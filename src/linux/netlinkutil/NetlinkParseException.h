// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "RuntimeErrorWithSourceLocation.h"
#include "Fwd.h"

class NetlinkParseException : public RuntimeErrorWithSourceLocation
{
public:
    NetlinkParseException(const NetlinkResponse& response, const std::string& reason, const std::source_location& source = std::source_location::current());

private:
    static std::string BuildMessage(const NetlinkResponse& response, const std::string& reason);
};
