// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <vector>
#include "NetlinkMessage.h"

class NetlinkResponse
{
public:
    using Titerator = std::vector<char>::const_iterator;

    NetlinkResponse(std::vector<char>&& content);

    void ThrowIfErrorFound() const;

    std::vector<char>::const_iterator Begin() const;
    std::vector<char>::const_iterator End() const;

    template <typename T>
    std::vector<NetlinkMessage<T>> Messages(int type) const;

    __u32 Sequence() const;

    bool MultiMessage() const;

    bool Done() const;

private:
    std::vector<char> m_data;
};

std::ostream& operator<<(std::ostream& out, const NetlinkResponse& response);

#include "NetlinkResponse.hxx"
