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
////////////////////////////////////////////////////////////////////////////////

#include "node_init.h"
#include "node_connection.h"
#include "node_request.h"
#include <iostream>
#include <fuerte/loop.h>

namespace arangodb { namespace fuerte { namespace js {

NAN_METHOD(poll){
  ::fu::poll();
}

NAN_METHOD(run){
  ::fu::run();
}

NAN_MODULE_INIT(InitAll) {
  std::cout << "About to init classes" << std::endl;
  NConnectionBuilder::Init(target);
  NConnection::Init(target);
  NRequest::Init(target);
  NResponse::Init(target);

  NAN_EXPORT(target, poll);
  NAN_EXPORT(target, run);
}

}}}

//
// Names the node and the function call to initialise
// the functionality it will provide
//
NODE_MODULE(arango_node_driver, ::arangodb::fuerte::js::InitAll);
