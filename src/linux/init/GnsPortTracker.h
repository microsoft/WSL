// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <memory>
#include <mutex>
#include "SocketChannel.h"
#include "util.h"

class GnsPortTracker
{
public:
    GnsPortTracker(std::shared_ptr<wsl::shared::SocketChannel> hvSocketChannel, std::shared_ptr<std::mutex> channelMutex);

    NON_COPYABLE(GnsPortTracker);
    NON_MOVABLE(GnsPortTracker);

    void Run();

    void RequestPort(void* data);

private:
    std::shared_ptr<wsl::shared::SocketChannel> m_hvSocketChannel;
    std::shared_ptr<std::mutex> m_channelMutex;
};
