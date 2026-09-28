// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <arpa/inet.h>
#include "Syscall.h"
#include "Utils.h"

template <typename T>
std::ostream& utils::FormatArray(std::ostream& out, const std::vector<T>& content)
{
    out << "[";

    for (size_t i = 0; i < content.size(); i++)
    {
        if (i != 0)
        {
            out << ",";
        }

        out << content[i];
    }

    return out << "]";
}

template <typename TAddr>
void utils::InitializeAddressAttribute(AddressAttribute<TAddr>& attribute, const Address& address, int type)
{
    // We can't pass &attribute.address because the structure is packed,
    // so that could cause an unaligned access
    TAddr netAddress = {};
    Syscall(::inet_pton, address.Family(), address.Addr().c_str(), &netAddress);

    attribute.header.rta_len = RTA_LENGTH(sizeof(netAddress));
    attribute.header.rta_type = type;
    attribute.address = netAddress;
}

inline void utils::InitializeCacheInfoAttribute(CacheInfoAttribute& attribute, const Address& address)
{
    attribute.header.rta_len = RTA_LENGTH(sizeof(struct ifa_cacheinfo));
    attribute.header.rta_type = IFA_CACHEINFO;
    attribute.cacheinfo.ifa_prefered = address.PreferredLifetime();
    attribute.cacheinfo.ifa_valid = 0xFFFFFFFF;
}

inline void utils::InitializeIntegerAttribute(IntegerAttribute& attribute, int value, int type)
{
    attribute.header.rta_len = RTA_LENGTH(sizeof(int));
    attribute.header.rta_type = type;
    attribute.value = value;
}

template <typename T>
std::string utils::Stringify(const T& value)
{
    std::stringstream str;
    str << value;
    return str.str();
}
