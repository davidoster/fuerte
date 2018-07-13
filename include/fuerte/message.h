////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
////////////////////////////////////////////////////////////////////////////////
#pragma once
#ifndef ARANGO_CXX_DRIVER_MESSAGE
#define ARANGO_CXX_DRIVER_MESSAGE

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/optional.hpp>

#include <velocypack/Buffer.h>
#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

#include "types.h"


namespace arangodb { namespace fuerte { inline namespace v1 {
const std::string fu_content_type_key("content-type");
const std::string fu_accept_key("accept");
  
struct MessageHeader {
  /// arangodb message format version
  short version() const { return _version; }
  void setVersion(short v) { _version = v; }

  
  /// Header meta data (equivalent to HTTP headers)
  StringMap meta;
  
#ifndef NDEBUG
  std::size_t byteSize;    // for debugging
#endif
  
public:
  
  // Header metadata helpers
  void addMeta(std::string const& key, std::string const& value);
  // Get value for header metadata key, returns empty string if not found.
  std::string metaByKey(std::string const& key) const;
  
  // content type accessors
  std::string contentTypeString() const;
  ContentType contentType() const;
  void contentType(std::string const& type);
  void contentType(ContentType type);
  
protected:
  short _version;
};
  
struct RequestHeader final : public MessageHeader {
  
  /// Database that is the target of the request
  std::string database;
  
  /// HTTP method
  RestVerb restVerb = RestVerb::Illegal;
  
  /// Local path of the request
  std::string path;
  
  /// Query parameters
  StringMap parameters;
  
public:
  
  // accept header accessors
  std::string acceptTypeString() const;
  ContentType acceptType() const;
  void acceptType(std::string const& type);
  void acceptType(ContentType type);
  
  // query parameter helpers
  void addParameter(std::string const& key, std::string const& value);
};
  
struct ResponseHeader final : public MessageHeader {
  friend class Response;

  /// Response code
  StatusCode responseCode = StatusUndefined;
  
  MessageType responseType() const { return _responseType; }
  
private:
  MessageType _responseType = MessageType::Response;
};

/*
struct AuthHeader : public MessageHeader {
  /// Authentication: encryption field
  AuthenticationType authType = AuthenticationType::None;
  /// Authentication: username
  std::string user;
  /// Authentication: password
  std::string password;
  /// Authentication: JWT token
  std::string token;

};*/

// Message is base class for message being send to (Request) or
// from (Response) a server.
class Message {
 protected:
  Message() = default;
  virtual ~Message() = default;

 public:
  /// Message type
  virtual MessageType type() const = 0;
  virtual MessageHeader const& messageHeader() const = 0;

  ///////////////////////////////////////////////
  // get payload
  ///////////////////////////////////////////////
  virtual std::vector<velocypack::Slice> const& slices() = 0;
  virtual boost::asio::const_buffer payload() const = 0;
  virtual size_t payloadSize() const = 0;
  std::string payloadAsString() const {
    auto p = payload();
    return std::string(boost::asio::buffer_cast<char const*>(p),
                       boost::asio::buffer_size(p));
  }

  // content-type header accessors
  std::string contentTypeString() const;
  ContentType contentType() const;
};

// Request contains the message send to a server in a request.
class Request final : public Message {
  static std::chrono::milliseconds _defaultTimeout;

 public:
  Request(RequestHeader&& messageHeader = RequestHeader())
      : header(std::move(messageHeader)),
        _sealed(false),
        _modified(true),
        _isVpack(boost::none),
        _builder(nullptr),
        _payloadLength(0),
        _timeout(std::chrono::duration_cast<std::chrono::milliseconds>(
            _defaultTimeout)) {}
  
  Request(RequestHeader const& messageHeader)
      : header(messageHeader),
        _sealed(false),
        _modified(true),
        _isVpack(boost::none),
        _builder(nullptr),
        _payloadLength(0),
        _timeout(std::chrono::duration_cast<std::chrono::milliseconds>(
            _defaultTimeout)) {}
  
  /// @brief request header
  RequestHeader header;
  
  MessageType type() const override { return MessageType::Request; }
  MessageHeader const& messageHeader() const override { return header; }
  
  ///////////////////////////////////////////////
  // header accessors
  ///////////////////////////////////////////////
  
  // accept header accessors
  std::string acceptTypeString() const;
  ContentType acceptType() const;

  ///////////////////////////////////////////////
  // add payload
  ///////////////////////////////////////////////
  void addVPack(velocypack::Slice const& slice);
  void addVPack(velocypack::Buffer<uint8_t> const& buffer);
  void addVPack(velocypack::Buffer<uint8_t>&& buffer);
  void addBinary(uint8_t const* data, std::size_t length);
  void addBinarySingle(velocypack::Buffer<uint8_t>&& buffer);

  ///////////////////////////////////////////////
  // get payload
  ///////////////////////////////////////////////
  std::vector<velocypack::Slice> const& slices() override;
  boost::asio::const_buffer payload() const override;
  size_t payloadSize() const override;

  // get timeout
  inline std::chrono::milliseconds timeout() const { return _timeout; }
  // set timeout
  void timeout(std::chrono::milliseconds timeout) { _timeout = timeout; }

 private:
  velocypack::Buffer<uint8_t> _payload;
  bool _sealed;
  bool _modified;
  ::boost::optional<bool> _isVpack;
  std::shared_ptr<velocypack::Builder> _builder;
  std::vector<velocypack::Slice> _slices;
  std::size_t _payloadLength;  // because VPackBuffer has quirks we need
                               // to track the Length manually
  std::chrono::milliseconds _timeout;
};

// Response contains the message resulting from a request to a server.
class Response final : public Message {
 public:
  Response(ResponseHeader&& reqHeader = ResponseHeader())
      : header(std::move(reqHeader)), _payloadOffset(0) {}
  
  /// @brief request header
  ResponseHeader header;
  
  MessageType type() const override { return header._responseType; }
  MessageHeader const& messageHeader() const override { return header; }
  ///////////////////////////////////////////////
  // get / check status
  ///////////////////////////////////////////////

  // statusCode returns the (HTTP) status code for the request (400==OK).
  StatusCode statusCode() { return header.responseCode; }
  // checkStatus returns true if the statusCode equals one of the given valid
  // code, false otherwise.
  bool checkStatus(std::initializer_list<StatusCode> validStatusCodes) {
    auto actual = statusCode();
    for (auto code : validStatusCodes) {
      if (code == actual) return true;
    }
    return false;
  }
  // assertStatus throw an exception if the statusCode does not equal one of the
  // given valid codes.
  void assertStatus(std::initializer_list<StatusCode> validStatusCodes) {
    if (!checkStatus(validStatusCodes)) {
      throw std::runtime_error("invalid status " +
                               std::to_string(statusCode()));
    }
  }

  ///////////////////////////////////////////////
  // get/set payload
  ///////////////////////////////////////////////
  bool isContentTypeJSON() const;
  bool isContentTypeVPack() const;
  bool isContentTypeHtml() const;
  bool isContentTypeText() const;
  std::vector<velocypack::Slice> const& slices() override;
  boost::asio::const_buffer payload() const override;
  size_t payloadSize() const override;

  void setPayload(velocypack::Buffer<uint8_t>&& buffer, size_t payloadOffset);

 private:
  velocypack::Buffer<uint8_t> _payload;
  size_t _payloadOffset;
  std::vector<velocypack::Slice> _slices;
};
}}}  // namespace arangodb::fuerte::v1
#endif
