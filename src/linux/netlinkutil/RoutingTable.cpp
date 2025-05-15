// Copyright (C) Microsoft Corporation. All rights reserved.
#include "RuntimeErrorWithSourceLocation.h"
#include "RoutingTable.h"
#include "NetlinkTransactionError.h"
#include "NetLinkStrings.h"
#include "Utils.h"
#include "common.h"

const Address c_ipv4LoopbackRouteSource = {AF_INET, 32, "127.0.0.1"};

RoutingTable::RoutingTable(int table) : m_table(table)
{
}

void RoutingTable::ChangeTableId(int newTableId)
{
    m_table = newTableId;
}

std::vector<Route> RoutingTable::ListRoutes(int family)
{
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected address family: {}", family));
    }

    std::vector<Route> routes;
    auto processRoute = [&](const NetlinkResponse& response) {
        for (const auto& e : response.Messages<rtmsg>(RTM_NEWROUTE))
        {
            const auto* message = e.Payload();
            auto tableId = e.UniqueAttribute<int>(RTA_TABLE);
            if ((family != AF_UNSPEC && family != message->rtm_family) || !tableId.has_value() || *tableId.value() != m_table)
            {
                continue;
            }

            auto readOptionalAddress = [&](int type) -> std::optional<Address> {
                auto attribute = e.UniqueAttribute<const void*>(type);
                if (!attribute.has_value())
                {
                    return {};
                }

                return Address::FromBinary(message->rtm_family, message->rtm_dst_len, attribute.value());
            };

            auto to = readOptionalAddress(RTA_DST);
            auto device = e.UniqueAttribute<int>(RTA_OIF);
            auto metric = e.UniqueAttribute<int>(RTA_PRIORITY);
            routes.emplace_back(
                message->rtm_family,
                readOptionalAddress(RTA_GATEWAY),
                device.has_value() ? *device.value() : -1,
                !to.has_value(),
                to,
                metric.has_value() ? *metric.value() : 0);
        }
    };

    rtmsg message{};
    message.rtm_family = family;
    auto transaction = m_channel.CreateTransaction(message, RTM_GETROUTE, NLM_F_DUMP);
    transaction.Execute(processRoute);

    return routes;
}

void RoutingTable::ModifyRoute(const Route& route, Operation action)
{
    if (route.family != AF_INET && route.family != AF_INET6)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected address family: {}", route.family));
    }

    assert(action == Operation::Create || action == Operation::Update || action == Operation::Remove);

    if (route.family == AF_INET)
    {
        ModifyRouteImpl<in_addr>(route, action);
    }
    else
    {
        ModifyRouteImpl<in6_addr>(route, action);
    }
}

template <typename TAddr>
void RoutingTable::ModifyRouteImpl(const Route& route, Operation action)
{
    int flags = 0;
    int operation = 0;
    if (action == Update)
    {
        flags = NLM_F_CREATE | NLM_F_REPLACE;
        operation = RTM_NEWROUTE;
    }
    else if (action == Create)
    {
        flags = NLM_F_CREATE;
        operation = RTM_NEWROUTE;
    }
    else
    {
        // In case of Remove, there are no additional flags needed besides NLM_F_REQUEST | NLM_F_ACK
        // which is set later in NetlinkChannel CreateTransaction
        operation = RTM_DELROUTE;
    }

    if (route.isLoopbackRoute)
    {
        ModifyLoopbackRouteImpl<TAddr>(route, operation, flags);
    }
    else if (route.defaultRoute)
    {
        ModifyDefaultRouteImpl<TAddr>(route, operation, flags);
    }
    else if (route.IsOnlink())
    {
        ModifyLinkLocalRouteImpl<TAddr>(route, operation, flags);
    }
    else
    {
        ModifyOfflinkRouteImpl<TAddr>(route, operation, flags);
    }
}

template <DerivedRouteMessage TMessage>
void RoutingTable::SendMessage(const Route& route, int operation, int flags, const std::function<void(TMessage&)>& routine)
{
    TMessage message{};
    message.route.rtm_family = route.family;
    message.route.rtm_table = RT_TABLE_UNSPEC; // == 0
    message.route.rtm_protocol = operation == RTM_DELROUTE ? RTPROT_UNSPEC : RTPROT_KERNEL;
    message.route.rtm_type = route.IsMulticast() ? RTN_MULTICAST : RTN_UNICAST;
    message.route.rtm_scope = route.IsOnlink() ? RT_SCOPE_LINK : RT_SCOPE_UNIVERSE;
    message.route.rtm_flags = RTM_F_NOTIFY;
    message.route.rtm_dst_len = route.to.has_value() ? route.to.value().PrefixLength() : 0;

    utils::InitializeIntegerAttribute(message.tableId, m_table, RTA_TABLE);
    utils::InitializeIntegerAttribute(message.dev, route.dev, RTA_OIF);

    routine(message);

    auto transaction = m_channel.CreateTransaction(message, operation, flags);
    try
    {
        transaction.Execute();
    }
    catch (const NetlinkTransactionError& transactionErr)
    {
        auto errorCode = transactionErr.Error();
        if (errorCode.has_value())
        {
            if (operation == RTM_DELROUTE)
            {
                // If the route already doesn't exist, we'll receive error "no such process".  Ignore that error and return success.
                if (errorCode.value() == -ESRCH)
                {
                    return;
                }
            }
            else
            {
                // Errors "file exists", "file not found", "no such process" are ignored in order to avoid keeping
                // track in GnsDaemon of what routes were added/updated and allow the same route to be
                // added/updated multiple times.
                if (errorCode.value() == -EEXIST || errorCode.value() == -ENOENT || errorCode.value() == -ESRCH)
                {
                    return;
                }
            }
        }

        throw;
    }
}

template <typename TAddr>
void RoutingTable::ModifyLoopbackRouteImpl(const Route& route, int operation, int flags)
{
    if (!route.to.has_value() || !route.via.has_value())
    {
        throw RuntimeErrorWithSourceLocation(std::format("Loopback route {} missing destination or gateway address", utils::Stringify(route)));
    }

    struct Message : RouteMessage
    {
        utils::AddressAttribute<TAddr> to;
        utils::AddressAttribute<TAddr> via;
        utils::AddressAttribute<TAddr> preferredSource;
    } __attribute__((packed));

    GNS_LOG_INFO(
        "SendMessage Route (to {}, via {}), operation ({}), netLinkflags ({})",
        route.to.has_value() ? route.to.value().Addr().c_str() : "[empty]",
        route.via.has_value() ? route.via.value().Addr().c_str() : "[empty]",
        RouteOperationToString(operation),
        NetLinkFormatFlagsToString(flags).c_str());

    SendMessage<Message>(route, operation, flags, [&](Message& message) {
        // For local IPs, the preferred source is set equal to destination. For a route to loopback address
        // range 127.0.0.0/8, preferred source is set to 127.0.0.1. This is done to ensure that when routing
        // loopback or local packets out of the guest, they won't have different source and destination
        // IPs, since they won't be accepted by the Windows host. Having the routes with source == destination is
        // also consistent with the how routes from the "local" routing table look like.
        //
        // Note: Since the IPv6 loopback range is ::1/128, we don't need separate code such as the one below
        // converting from 127.0.0.0 to 127.0.0.1.
        if (route.to.value().Addr().compare("127.0.0.0") == 0)
        {
            GNS_LOG_INFO(
                "InitializeAddressAttribute (preferred source address) RTA_PREFSRC to {}", c_ipv4LoopbackRouteSource.Addr().c_str());
            utils::InitializeAddressAttribute<TAddr>(message.preferredSource, c_ipv4LoopbackRouteSource, RTA_PREFSRC);
        }
        else
        {
            // Set the preferred source address to be the same as the route destination.
            GNS_LOG_INFO("InitializeAddressAttribute (preferred source address) RTA_PREFSRC to {}", route.to.value().Addr().c_str());
            utils::InitializeAddressAttribute<TAddr>(message.preferredSource, route.to.value(), RTA_PREFSRC);
        }

        message.route.rtm_flags |= RTNH_F_ONLINK;
        GNS_LOG_INFO(
            "InitializeAddressAttribute RTA_DST ({}) RTA_GATEWAY ({}) RTA_PRIORITY ([not set])",
            route.to.value().Addr().c_str(),
            route.via.value().Addr().c_str());
        utils::InitializeAddressAttribute<TAddr>(message.to, route.to.value(), RTA_DST);
        utils::InitializeAddressAttribute<TAddr>(message.via, route.via.value(), RTA_GATEWAY);
    });
}

template <typename TAddr>
void RoutingTable::ModifyDefaultRouteImpl(const Route& route, int operation, int flags)
{
    if (!route.via.has_value())
    {
        throw RuntimeErrorWithSourceLocation("Default route is missing its gateway address");
    }

    struct Message : RouteMessage
    {
        utils::AddressAttribute<TAddr> via;
        utils::IntegerAttribute metric;
    } __attribute__((packed));

    GNS_LOG_INFO(
        "SendMessage Route (to {}, via {}), operation ({}), netLinkflags ({})",
        route.to.has_value() ? route.to.value().Addr().c_str() : "[empty]",
        route.via.has_value() ? route.via.value().Addr().c_str() : "[empty]",
        RouteOperationToString(operation),
        NetLinkFormatFlagsToString(flags).c_str());

    SendMessage<Message>(route, operation, flags, [&](Message& message) {
        GNS_LOG_INFO(
            "InitializeAddressAttribute RTA_DST ([not set]) RTA_GATEWAY ({}), RTA_PRIORITY ({})",
            route.to.has_value() ? route.to.value().Addr().c_str() : "[empty]",
            route.metric);
        utils::InitializeAddressAttribute<TAddr>(message.via, route.via.value(), RTA_GATEWAY);
        utils::InitializeIntegerAttribute(message.metric, route.metric, RTA_PRIORITY);
    });
}

template <typename TAddr>
void RoutingTable::ModifyLinkLocalRouteImpl(const Route& route, int operation, int flags)
{
    struct Message : RouteMessage
    {
        utils::AddressAttribute<TAddr> to;
        utils::IntegerAttribute metric;
    } __attribute__((packed));

    GNS_LOG_INFO(
        "SendMessage Route (to {}, via {}), operation ({}), netLinkflags ({})",
        route.to.has_value() ? route.to.value().Addr().c_str() : "[empty]",
        route.via.has_value() ? route.via.value().Addr().c_str() : "[empty]",
        RouteOperationToString(operation),
        NetLinkFormatFlagsToString(flags).c_str());

    SendMessage<Message>(route, operation, flags, [&](Message& message) {
        GNS_LOG_INFO(
            "InitializeAddressAttribute RTA_DST ({}) RTA_GATEWAY ([not set]), RTA_PRIORITY ({})",
            route.to.has_value() ? route.to.value().Addr().c_str() : "[empty]",
            route.metric);
        utils::InitializeAddressAttribute<TAddr>(message.to, route.to.value(), RTA_DST);
        utils::InitializeIntegerAttribute(message.metric, route.metric, RTA_PRIORITY);
    });
}

template <typename TAddr>
void RoutingTable::ModifyOfflinkRouteImpl(const Route& route, int operation, int flags)
{
    if (!route.via.has_value())
    {
        throw RuntimeErrorWithSourceLocation("Offlink route is missing its next hop");
    }

    struct Message : RouteMessage
    {
        utils::AddressAttribute<TAddr> to;
        utils::AddressAttribute<TAddr> via;
        utils::IntegerAttribute metric;
    } __attribute__((packed));

    GNS_LOG_INFO(
        "SendMessage Route (to {}, via {}), operation ({}), netLinkflags ({})",
        route.to.has_value() ? route.to.value().Addr().c_str() : "[empty]",
        route.via.has_value() ? route.via.value().Addr().c_str() : "[empty]",
        RouteOperationToString(operation),
        NetLinkFormatFlagsToString(flags).c_str());

    SendMessage<Message>(route, operation, flags, [&](Message& message) {
        GNS_LOG_INFO(
            "InitializeAddressAttribute RTA_DST ({}) RTA_GATEWAY ({}), RTA_PRIORITY ({})",
            route.to.has_value() ? route.to.value().Addr().c_str() : "[empty]",
            route.via.has_value() ? route.via.value().Addr().c_str() : "[empty]",
            route.metric);
        utils::InitializeAddressAttribute<TAddr>(message.to, route.to.value(), RTA_DST);
        utils::InitializeAddressAttribute<TAddr>(message.via, route.via.value(), RTA_GATEWAY);
        utils::InitializeIntegerAttribute(message.metric, route.metric, RTA_PRIORITY);
    });
}

// Delete all routes from the specified address family
void RoutingTable::RemoveAll(int addressFamily)
{
    for (const auto& route : ListRoutes(addressFamily))
    {
        ModifyRoute(route, Remove);
    }
}
