////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016-2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Christoph Uhde
/// @author Ewout Prangsma
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "VstConnection.h"

#include "Basics/cpu-relax.h"
#include "vst.h"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <fuerte/FuerteLogger.h>
#include <fuerte/helper.h>
#include <fuerte/loop.h>
#include <fuerte/message.h>
#include <fuerte/types.h>
#include <velocypack/velocypack-aliases.h>

namespace fu = arangodb::fuerte::v1;
using namespace arangodb::fuerte::v1::vst;

using bt = ::boost::asio::ip::tcp;
using be = ::boost::asio::ip::tcp::endpoint;
using BoostEC = ::boost::system::error_code;
using RequestItemSP = std::shared_ptr<RequestItem>;

VstConnection::VstConnection(
    std::shared_ptr<boost::asio::io_context> const& ctx,
    fu::detail::ConnectionConfiguration const& configuration)
    : AsioConnection(ctx, configuration),
      _vstVersion(configuration._vstVersion) {}

// Deconstruct.
VstConnection::~VstConnection() {}

static std::atomic<MessageID> vstMessageId(1);
// sendRequest prepares a RequestItem for the given parameters
// and adds it to the send queue.
MessageID VstConnection::sendRequest(std::unique_ptr<Request> request,
                                     RequestCallback cb) {
  // it does not matter if IDs are reused on different connections
  uint64_t mid = vstMessageId.fetch_add(1, std::memory_order_relaxed);
  
  // Create RequestItem from parameters
  std::unique_ptr<RequestItem> item(new RequestItem());
  item->_messageID = mid;
  item->_callback = cb;
  item->_request = std::move(request);
  item->prepareForNetwork(_vstVersion);
  
  // Add item to send queue
  uint32_t state = queueRequest(std::move(item));

  // this allows sendRequest to return immediately and
  // not to block until all writing is done
  if (_connected.load(std::memory_order_acquire)) {
    FUERTE_LOG_VSTTRACE << "sendRequest (vst): start sending & reading"
                        << std::endl;
    if (!(state & WRITE_LOOP_ACTIVE)) {
      startWriting();
    }
  } else {
    FUERTE_LOG_VSTTRACE << "sendRequest (vst): not connected" << std::endl;
  }
  return mid;
}

std::size_t VstConnection::requestsLeft() const {
  // this function does not return the exact size (both mutexes would be
  // required to be locked at the same time) but as it is used to decide
  // if another run is called or not this should not be critical.
  return AsioConnection::requestsLeft() + _messageStore.size();
};

// socket connection is up (with optional SSL), now initiate the VST protocol.
void VstConnection::finishInitialization() {
  FUERTE_LOG_CALLBACKS << "finishInitialization (vst)" << std::endl;

  const char* vstHeader;
  switch (_vstVersion) {
    case VST1_0:
      vstHeader = vstHeader1_0;
      break;
    case VST1_1:
      vstHeader = vstHeader1_1;
      break;
    default:
      throw std::logic_error("Unknown VST version");
  }

  auto self = shared_from_this();
  boost::asio::async_write(
      *_socket, boost::asio::buffer(vstHeader, strlen(vstHeader)),
      [this, self](BoostEC const& error, std::size_t transferred) {
        if (error) {
          FUERTE_LOG_ERROR << error.message() << std::endl;
          _connected.store(false, std::memory_order_release);
          shutdownConnection(ErrorCondition::WriteError);
          onFailure(
              errorToInt(ErrorCondition::CouldNotConnect),
              "unable to initialize connection: error=" + error.message());
        } else {
          FUERTE_LOG_CALLBACKS
              << "VST connection established; starting send/read loop"
              << std::endl;
          if (_configuration._authenticationType != AuthenticationType::None) {
            // send the auth, then set _connected == true
            sendAuthenticationRequest();
          } else {
            _connected.store(true, std::memory_order_release);
            startWriting(); // start writing if something is queued
          }
        }
      });
}

// Send out the authentication message on this connection
void VstConnection::sendAuthenticationRequest() {
  assert(_configuration._authenticationType != AuthenticationType::None);
  
  // Part 1: Build ArangoDB VST auth message (1000)
  auto item = std::make_shared<RequestItem>();
  item->_request = nullptr; // should not break anything
  item->_messageID = vstMessageId.fetch_add(1, std::memory_order_relaxed);
  
  if (_configuration._authenticationType == AuthenticationType::Basic) {
    item->_requestMetadata = vst::message::authBasic(_configuration._user, _configuration._password);
  } else if (_configuration._authenticationType == AuthenticationType::Jwt) {
    item->_requestMetadata = vst::message::authJWT(_configuration._jwtToken);
  }
  assert(item->_requestMetadata.size() < defaultMaxChunkSize);
  boost::asio::const_buffer header(item->_requestMetadata.data(),
                                   item->_requestMetadata.byteSize());

  item->prepareForNetwork(_vstVersion, header, boost::asio::const_buffer(0,0));

  auto self = shared_from_this();
  item->_callback = [this, self](Error error, std::unique_ptr<Request>,
                                 std::unique_ptr<Response> resp) {
    if (error || resp->statusCode() != StatusOK) {
      _permanent_failure = true;
      onFailure(error, "authentication failed");
    }
  };
  // add message to store
  _messageStore.add(item);
  
  // actually send auth request
  boost::asio::post(*_io_context, [this, self, item] {
    auto cb = [this, self, item](BoostEC const& e, std::size_t transferred) {
      _connected.store(true, std::memory_order_release);
      asyncWriteCallback(e, transferred, std::move(item)); // calls startReading()
      startWriting(); // start writing if something was queued
    };
    if (_configuration._ssl) {
      boost::asio::async_write(*_sslSocket, item->_requestBuffers, cb);
    } else {
      boost::asio::async_write(*_socket, item->_requestBuffers, cb);
    }
  });
}

// ------------------------------------
// Writing data
// ------------------------------------

// fetch the buffers for the write-loop (called from IO thread)
std::vector<boost::asio::const_buffer> VstConnection::fetchBuffers(
    std::shared_ptr<RequestItem> const& next) {
  _messageStore.add(next);  // Add item to message store
  startReading();           // Make sure we're listening for a response
  return next->_requestBuffers;
}

// Thread-Safe: activate the writer loop (if off and items are queud)
void VstConnection::startWriting() {
  assert(_connected);
  FUERTE_LOG_TRACE << "startWriting (vst): this=" << this << std::endl;

  uint32_t state = _loopState.load(std::memory_order_acquire);
  // start the loop if necessary
  while (!(state & WRITE_LOOP_ACTIVE) && (state & WRITE_LOOP_QUEUE_MASK) > 0) {
    if (_loopState.compare_exchange_weak(state, state | WRITE_LOOP_ACTIVE,
                                         std::memory_order_seq_cst)) {
      FUERTE_LOG_TRACE << "startWriting (vst): starting write\n";
      // auto self = shared_from_this();
      //_io_context->post([this, self] {
      asyncWrite();
      //});
      return;
    }
    cpu_relax();
  }
  if ((state & WRITE_LOOP_QUEUE_MASK) == 0) {
    FUERTE_LOG_TRACE << "startWriting (vst): nothing is queued\n";
  }
}

// callback of async_write function that is called in sendNextRequest.
void VstConnection::asyncWriteCallback(::boost::system::error_code const& error,
                                       std::size_t transferred,
                                       std::shared_ptr<RequestItem> item) {
  _timeout.cancel();

  // auto pendingAsyncCalls = --_connection->_async_calls;
  if (error) {
    // Send failed
    FUERTE_LOG_CALLBACKS << "asyncWriteCallback (vst): error "
                         << error.message() << std::endl;
    FUERTE_LOG_ERROR << error.message() << std::endl;

    // Item has failed, remove from message store
    _messageStore.removeByID(item->_messageID);

    // let user know that this request caused the error
    item->_callback.invoke(errorToInt(ErrorCondition::WriteError),
                           std::move(item->_request), nullptr);

    // Stop current connection and try to restart a new one.
    // This will reset the current write loop.
    restartConnection(ErrorCondition::WriteError);

  } else {
    // Send succeeded
    FUERTE_LOG_CALLBACKS << "asyncWriteCallback (vst): send succeeded, "
                         << transferred << " bytes transferred\n";
    // async-calls=" << pendingAsyncCalls << std::endl;

    // request is written we no longer need data for that
    item->resetSendData();

    // check the queue length, stop write loop if necessary
    uint32_t state = _loopState.load(std::memory_order_seq_cst);
    // nothing is queued, lets try to halt the write queue while
    // the write loop is active and nothing is queued
    while ((state & WRITE_LOOP_ACTIVE) &&
           (state & WRITE_LOOP_QUEUE_MASK) == 0) {
      if (_loopState.compare_exchange_weak(state, state & ~WRITE_LOOP_ACTIVE)) {
        FUERTE_LOG_TRACE << "asyncWrite: no more queued items" << std::endl;
        state = state & ~WRITE_LOOP_ACTIVE;
        break;  // we turned flag off while nothin was queued
      }
      cpu_relax();
    }

    if (!(state & READ_LOOP_ACTIVE)) {
      startReading();  // Make sure we're listening for a response
    }

    // Continue with next request (if any)
    FUERTE_LOG_CALLBACKS
        << "asyncWriteCallback (vst): send next request (if any)" << std::endl;

    if (state & WRITE_LOOP_ACTIVE) {
      asyncWrite();  // continue writing
    }
  }
}

// handler for deadline timer
/*void VstConnection::WriteLoop::deadlineHandler(const
 boost::system::error_code& error) {
 if (!error) {
 // Stop current connection and try to restart a new one.
 // This will reset the current write loop.
 _connection->restartConnection(this, ErrorCondition::Timeout);
 }
 }*/

// ------------------------------------
// Reading data
// ------------------------------------

// Thread-Safe: activate the read loop (if needed)
void VstConnection::startReading() {
  FUERTE_LOG_VSTTRACE << "startReading: this=" << this << std::endl;

  uint32_t state = _loopState.load(std::memory_order_seq_cst);
  // start the loop if necessary
  while (!(state & READ_LOOP_ACTIVE)) {
    if (_loopState.compare_exchange_weak(state, state | READ_LOOP_ACTIVE,
                                         std::memory_order_seq_cst)) {
      // auto self = shared_from_this();
      //_io_context->post([this, self] {
      asyncReadSome();
      //});
      return;
    }
    cpu_relax();
  }
  // There is already a read loop, do nothing
}

// Thread-Safe: Stop the read loop
void VstConnection::stopReading() {
  FUERTE_LOG_VSTTRACE << "stopReading: this=" << this << std::endl;

  uint32_t state = _loopState.load(std::memory_order_relaxed);
  // start the loop if necessary
  while (state & READ_LOOP_ACTIVE) {
    if (_loopState.compare_exchange_weak(state, state & ~READ_LOOP_ACTIVE,
                                         std::memory_order_seq_cst)) {
      return;
    }
  }
}

// asyncReadCallback is called when asyncReadSome is resulting in some data.
void VstConnection::asyncReadCallback(const boost::system::error_code& e,
                                      std::size_t transferred) {
  _timeout.cancel();

  // auto pendingAsyncCalls = --_connection->_async_calls;
  if (e) {
    FUERTE_LOG_CALLBACKS
        << "asyncReadCallback: Error while reading form socket";
    FUERTE_LOG_ERROR << e.message() << std::endl;

    // Restart connection, this will trigger a release of the readloop.
    restartConnection(ErrorCondition::VstReadError);

  } else {
    FUERTE_LOG_CALLBACKS
        << "asyncReadCallback: received " << transferred
        << " bytes\n";  // async-calls=" << pendingAsyncCalls << std::endl;

    // Inspect the data we've received so far.
    /*auto buffers = _receiveBuffer.data(); // no copy
    for (auto const& buffer : buffers) {
      buffer.data()
    }*/

    // Inspect the data we've received so far.
    auto recvBuffs = _receiveBuffer.data();  // no copy
    auto cursor = boost::asio::buffer_cast<const uint8_t*>(recvBuffs);
    auto available = boost::asio::buffer_size(recvBuffs);
    // TODO technically buffer_cast is deprecated
    
    size_t parsedBytes = 0;
    while (vst::parser::isChunkComplete(cursor, available)) {
      // Read chunk
      ChunkHeader chunk;
      switch (_vstVersion) {
        case VST1_0:
          chunk = vst::parser::readChunkHeaderVST1_0(cursor);
          break;
        case VST1_1:
          chunk = vst::parser::readChunkHeaderVST1_1(cursor);
          break;
        default:
          throw std::logic_error("Unknown VST version");
      }

      // Process chunk
      processChunk(chunk);

      cursor += chunk.chunkLength();
      available -= chunk.chunkLength();
      parsedBytes += chunk.chunkLength();
    }
    
    // Remove consumed data from receive buffer.
    _receiveBuffer.consume(parsedBytes);

    // check for more messages that could arrive
    if (_messageStore.empty(true) &&
        !(_loopState.load(std::memory_order_acquire) & WRITE_LOOP_ACTIVE)) {
      FUERTE_LOG_VSTTRACE << "shouldStopReading: no more pending "
                             "messages/requests, stopping read";
      stopReading();
      return;  // write-loop restarts read-loop if necessary
    }

// Set timeout
/*std::chrono::milliseconds timeout = _messageStore.minimumTimeout(true);
assert(timeout.count() > 0);
auto self = shared_from_this();
_timeout.expires_from_now(timeout);
_timeout.async_wait(std::bind(&VstConnection::timeoutExpired,
std::static_pointer_cast<VstConnection>(self), std::placeholders::_1));*/
#warning fix

    asyncReadSome();  // Continue read loop
  }
}

// handler for deadline timer
/*void VstConnection::ReadLoop::deadlineHandler(const boost::system::error_code&
error) {
  if (!error) {
    // Stop current connection and try to restart a new one.
    // This will reset the current write loop.
    _connection->restartConnection(this, ErrorCondition::Timeout);
  }
}*/

// Process the given incoming chunk.
void VstConnection::processChunk(ChunkHeader& chunk) {
  auto msgID = chunk.messageID();
  FUERTE_LOG_VSTTRACE << "processChunk: messageID=" << msgID << std::endl;

  // Find requestItem for this chunk.
  auto item = _messageStore.findByID(chunk._messageID);
  if (!item) {
    FUERTE_LOG_ERROR << "got chunk with unknown message ID: " << msgID
                     << std::endl;
    return;
  }

  // We've found the matching RequestItem.
  item->addChunk(chunk);

  // Try to assembly chunks in RequestItem to complete response.
  auto completeBuffer = item->assemble();
  if (completeBuffer) {
    FUERTE_LOG_VSTTRACE << "processChunk: complete response received"
                        << std::endl;
    // Message is complete
    // Remove message from store
    _messageStore.removeByID(item->_messageID);

    // Create response
    auto response = createResponse(*item, completeBuffer);
    if (response == nullptr) {
      item->_callback.invoke(errorToInt(ErrorCondition::ProtocolError),
                             std::move(item->_request), nullptr);
      // Notify listeners
      FUERTE_LOG_VSTTRACE
      << "processChunk: notifying RequestItem error callback"
      << std::endl;
      return;
    }

    // Notify listeners
    FUERTE_LOG_VSTTRACE
        << "processChunk: notifying RequestItem success callback"
        << std::endl;
    item->_callback.invoke(0, std::move(item->_request), std::move(response));
  }
}

// Create a response object for given RequestItem & received response buffer.
std::unique_ptr<fu::Response> VstConnection::createResponse(
    RequestItem& item, std::unique_ptr<VPackBuffer<uint8_t>>& responseBuffer) {
  FUERTE_LOG_VSTTRACE << "creating response for item with messageid: "
                      << item._messageID << std::endl;
  auto itemCursor = responseBuffer->data();
  auto itemLength = responseBuffer->byteSize();
  
  // first part of the buffer contains the response buffer
  std::size_t headerLength;
  MessageType type = parser::validateAndExtractMessageType(itemCursor, itemLength, headerLength);
  if (type != MessageType::Response) {
    FUERTE_LOG_ERROR << "received unsupported vst message from server";
    return nullptr;
  }
  
  ResponseHeader header = parser::responseHeaderFromSlice(VPackSlice(itemCursor));
  auto response = std::unique_ptr<Response>(new Response(std::move(header)));
  response->setPayload(std::move(*responseBuffer), /*offset*/headerLength);

  return response;
}

// called when the timeout expired
void VstConnection::timeoutExpired(boost::system::error_code const& e) {
  if (!e) {  // expired
  }
}
