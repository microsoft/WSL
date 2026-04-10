// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <memory>
#include "SocketChannel.h"
#include "util.h"
#include "bind_monitor.h"

class GnsPortTracker
{
public:
    GnsPortTracker(std::shared_ptr<wsl::shared::SocketChannel> hvSocketChannel);

    NON_COPYABLE(GnsPortTracker);
    NON_MOVABLE(GnsPortTracker);

    void Run();

    void RequestPort(const bind_event& Event);

private:
    std::shared_ptr<wsl::shared::SocketChannel> m_hvSocketChannel;
};
