// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <functional>
#include "NetlinkChannel.h"
#include "Neighbor.h"
#include "Operation.h"

class IpNeighborManager
{
public:
    /*
        Implement netlink equivalent of "ip neigh <operation> <IP> lladdr <MAC> dev <interface> nud permanent".
    */
    void ModifyNeighborEntry(const Neighbor& Neighbor, Operation operation);

    static bool PerformNeighborDiscovery(Neighbor& Local, Neighbor& Neighbor);

private:
    template <typename TAddr>
    void ModifyNeighborEntryImpl(const Neighbor& Neighbor, int operation, int flags);

    /*
        Creates the message using the routine, then sends the message via netlink.
    */
    template <typename T>
    void SendMessage(const Neighbor& Neighbor, int operation, int flags, const std::function<void(T&)>& routine = [](auto&) {});

    NetlinkChannel m_channel;
};
