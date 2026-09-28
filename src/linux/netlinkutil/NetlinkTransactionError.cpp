// Copyright (C) Microsoft Corporation. All rights reserved.
#include <sstream>
#include "NetlinkTransactionError.h"
#include "NetlinkError.h"
#include "NetlinkResponse.h"
#include "Utils.h"

NetlinkTransactionError::NetlinkTransactionError(
    const std::vector<char>& request, const std::vector<NetlinkResponse>& responses, const std::exception& inner, const std::source_location& source) :
    RuntimeErrorWithSourceLocation(BuildMessage(request, responses, inner), source)
{
    const auto* error = dynamic_cast<const NetlinkError*>(&inner);
    if (error != nullptr)
    {
        m_error = error->Error();
    }
}

std::optional<int> NetlinkTransactionError::Error() const
{
    return m_error;
}

std::string NetlinkTransactionError::BuildMessage(const std::vector<char>& request, const std::vector<NetlinkResponse>& responses, const std::exception& inner)
{
    std::stringstream str;

    str << "Error in netlink transaction.\n";
    str << "Innermost exception: " << inner.what();
    str << "\nRequest: ";
    utils::FormatBinary(str, request.data(), request.size());
    str << "\nResponses: (seen: " << std::to_string(responses.size()) << ") ";
    utils::FormatArray(str, responses);

    return str.str();
}
