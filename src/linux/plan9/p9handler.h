// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9platform.h"
#include "p9ihandler.h"

namespace p9fs {

// This class is basically a countdown event for active connections.
class WaitGroup
{
public:
    // Add a new wait group member. This should only be called by HandleConnections.
    auto Add()
    {
        ++m_Count;
        return std::unique_ptr<WaitGroup, Deleter>{this};
    }

    // Wait until all members are done. This should only be called by HandleConnections.
    Task<void> Wait()
    {
        Done();
        co_await m_Event;
    }

    // Check if there are members.
    // N.B. The primary member (which is released by HandleConnections when the cancel token is
    //      canceled), is not counted.
    // N.B. This is the only member the caller of HandleConnections may use.
    bool HasMembers() const noexcept
    {
        return m_Count > 1;
    }

private:
    void Done()
    {
        if (--m_Count == 0)
        {
            m_Event.Set();
        }
    }

    struct Deleter
    {
        void operator()(WaitGroup* group) const
        {
            group->Done();
        }
    };

    AsyncEvent m_Event;
    std::atomic<ULONG_PTR> m_Count{1};
};

AsyncTask HandleConnections(ISocket& listen, IShareList& shareList, CancelToken& token, WaitGroup& waitGroup);

} // namespace p9fs
