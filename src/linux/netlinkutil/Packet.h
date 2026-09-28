// Copyright (C) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once
#include <vector>

class Packet
{
public:
    static constexpr long InitialReservedHeader = 128;
    static constexpr long InitialPacketSize = 2048;

    // Initial state is data_offset==data_end_offset
    void reset()
    {
        data_offset = InitialReservedHeader;
        data_end_offset = data_offset + InitialPacketSize;
        Buffer.resize(InitialReservedHeader + InitialPacketSize);
    }

    uint8_t* data()
    {
        return &Buffer[data_offset];
    }

    uint8_t* data_end()
    {
        return &Buffer[data_end_offset];
    }

    bool adjust_head(long count)
    {
        if ((count + data_offset) < 0)
        {
            return false;
        }

        if ((count + data_offset) > data_end_offset)
        {
            return false;
        }

        data_offset += count;
        return true;
    }

    bool adjust_tail(long count)
    {
        if ((count + data_end_offset) < data_offset)
        {
            return false;
        }
        if ((count + data_end_offset) > Buffer.size())
        {
            Buffer.resize(count + data_end_offset);
        }

        data_end_offset += count;
        return true;
    }

private:
    long data_offset = 0;
    long data_end_offset = 0;
    std::vector<uint8_t> Buffer;
};
