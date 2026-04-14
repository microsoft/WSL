// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <memory>
#include "SocketChannel.h"
#include "util.h"

class GnsPortTracker
{
public:
    GnsPortTracker(std::shared_ptr<wsl::shared::SocketChannel> hvSocketChannel);

    NON_COPYABLE(GnsPortTracker);
    NON_MOVABLE(GnsPortTracker);

    void Run();

    void RequestPort(void* data);

private:
    std::shared_ptr<wsl::shared::SocketChannel> m_hvSocketChannel;
};
