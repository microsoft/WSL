// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "RuntimeErrorWithSourceLocation.h"
#include "NetlinkMessage.h"

class NetlinkTransactionError : public RuntimeErrorWithSourceLocation
{
public:
    NetlinkTransactionError(
        const std::vector<char>& request,
        const std::vector<NetlinkResponse>& responses,
        const std::exception& inner,
        const std::source_location& source = std::source_location::current());

    std::optional<int> Error() const;

private:
    static std::string BuildMessage(const std::vector<char>& request, const std::vector<NetlinkResponse>& responses, const std::exception& inner);

    std::optional<int> m_error;
};
