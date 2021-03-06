/*  Copyright (C) 2014-2018 FastoGT. All right reserved.

    This file is part of FastoTV.

    FastoTV is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    FastoTV is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with FastoTV. If not, see <http://www.gnu.org/licenses/>.
*/

#include "inner/inner_client.h"

namespace fastotv {
namespace inner {

InnerClient::InnerClient(common::libev::IoLoop* server, const common::net::socket_info& info)
    : common::libev::tcp::TcpClient(server, info) {}

InnerClient::~InnerClient() {}

const char* InnerClient::ClassName() const {
  return "InnerClient";
}

ProtocoledInnerClient::ProtocoledInnerClient(common::libev::IoLoop* server, const common::net::socket_info& info)
    : base_class(server, info) {}

}  // namespace inner
}  // namespace fastotv
