// Copyright (C) Microsoft Corporation. All rights reserved.

#include "Utils.h"
#include "RuntimeErrorWithSourceLocation.h"
#include <iomanip>
#include <sstream>
#include <string.h>

std::ostream& utils::FormatBinary(std::ostream& out, const void* ptr, size_t bytes)
{
    out << "(" << bytes << " bytes) {";
    out << BytesToHex(ptr, bytes, ",");

    return out << "}";
}

std::string utils::BytesToHex(const void* ptr, size_t bytes, const std::string& separator)
{
    std::stringstream out;
    for (size_t i = 0; i < bytes; i++)
    {
        if (i != 0)
        {
            out << separator;
        }

        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(reinterpret_cast<const std::uint8_t*>(ptr)[i]);
    }

    return out.str();
}

Address utils::ComputeBroadcastAddress(const Address& address)
{
    if (address.Family() != AF_INET)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Can't compute broadcast address for address family: {}", address.Family()));
    }

    auto bytes = address.AsBytes<in_addr>();

    // Set all the bits between the prefixLength and 32 to 1
    std::uint32_t suffix = (1 << (32 - address.PrefixLength())) - 1;
    bytes.s_addr |= htonl(suffix);

    return Address::FromBytes(AF_INET, address.PrefixLength(), bytes);
}
