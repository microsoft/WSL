// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <functional>
#include "NetlinkChannel.h"
#include "Route.h"
#include "Operation.h"

struct RouteMessage
{
    rtmsg route;
    utils::IntegerAttribute tableId;
    utils::IntegerAttribute dev;
} __attribute__((packed));

// The below is a means to ensure that messages have a common set of fields.  It just means
// that a type T must inherit from RouteMessage, and any functions that reference
// DerivedRouteMessage can be assured that they can safely access the fields in RouteMessage.
template <typename TMessage>
concept DerivedRouteMessage = std::is_base_of<RouteMessage, TMessage>::value;

class RoutingTable
{
public:
    RoutingTable(int table);
    RoutingTable(const RoutingTable&) = delete;
    RoutingTable(RoutingTable&&) = delete;

    const RoutingTable& operator=(const RoutingTable&) const = delete;
    const RoutingTable& operator=(RoutingTable&&) const = delete;

    void ChangeTableId(int newTableId);

    void ModifyRoute(const Route& route, Operation action);

    std::vector<Route> ListRoutes(int family);

    void RemoveAll(int addressFamily);

private:
    /*
        Implement netlink equivalent of
        "ip route <operation> table <id> <destination> via <gateway> src <source> dev <interface> onlink".

        Note: the preferred source is set equal to destination. See comments in method implementation for more
        details.
    */
    template <typename TAddr>
    void ModifyLoopbackRouteImpl(const Route& route, int operation, int flags);

    template <typename TAddr>
    void ModifyDefaultRouteImpl(const Route& route, int operation, int flags);

    template <typename TAddr>
    void ModifyLinkLocalRouteImpl(const Route& route, int operation, int flags);

    /*
        By "offlink route", we mean a route with a specific destination prefix and a specific next hop.

        Contrast this to a default route which has no specific destination prefix and a link local
        route which has no specific next hop.
    */
    template <typename TAddr>
    void ModifyOfflinkRouteImpl(const Route& route, int operation, int flags);

    template <typename TAddr>
    void ModifyRouteImpl(const Route& route, Operation action);

    /*
        Creates the message using the routine, then sends the message via netlink.
    */
    template <DerivedRouteMessage TMessage>
    void SendMessage(const Route& route, int operation, int flags, const std::function<void(TMessage&)>& routine = [](auto&) {});

    NetlinkChannel m_channel;
    int m_table;
};
