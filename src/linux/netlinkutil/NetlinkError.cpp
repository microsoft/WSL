// Copyright (C) Microsoft Corporation. All rights reserved.
#include <format>
#include <string>
#include "NetlinkError.h"

NetlinkError::NetlinkError(int error, const std::source_location& source) :
    RuntimeErrorWithSourceLocation(std::format("Netlink returned error: {}", error), source), m_error(error)
{
}

int NetlinkError::Error() const
{
    return m_error;
}
