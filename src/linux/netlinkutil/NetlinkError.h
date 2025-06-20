// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "RuntimeErrorWithSourceLocation.h"

class NetlinkError : public RuntimeErrorWithSourceLocation
{
public:
    NetlinkError(int error, const std::source_location& source = std::source_location::current());

    int Error() const;

private:
    int m_error = -1;
};
