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
/// @author John Bufton
////////////////////////////////////////////////////////////////////////////////

#include <fuerte/old/HeaderLoader.h>

namespace arangodb {

namespace dbinterface {

namespace Header {

Common& Loader::operator()(const uint8_t* ptr) {
  Common::ChunkInfo info = Common::chnkInfo(ptr);
  Common* pHeader;
  if (Common::bSingleChunk(info)) {
    pHeader = new (_hdr._single) Single{ptr};
    return *pHeader;
  }
  if (Common::bFirstChunk(info)) {
    pHeader = new (_hdr._multi) Multi{ptr};
    return *pHeader;
  }
  pHeader = new (_hdr._common) Common{ptr};
  return *pHeader;
}
}
}
}
