// Copyright (C) Microsoft Corporation. All rights reserved.
#include <sstream>
#include "NetlinkParseException.h"
#include "NetlinkResponse.h"
#include "Utils.h"

NetlinkParseException::NetlinkParseException(const NetlinkResponse& response, const std::string& reason, const std::source_location& source) :
    RuntimeErrorWithSourceLocation(BuildMessage(response, reason), source)
{
}

std::string NetlinkParseException::BuildMessage(const NetlinkResponse& response, const std::string& reason)
{
    std::stringstream str;
    str << reason << " Netlink response: ";
    utils::FormatBinary(str, &*response.Begin(), response.End() - response.Begin());

    return str.str();
}
