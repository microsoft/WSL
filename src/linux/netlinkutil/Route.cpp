// Copyright (C) Microsoft Corporation. All rights reserved.
#include <iostream>
#include "Route.h"
#include "Utils.h"

Route::Route(int routeFamily, const std::optional<Address>& routeNextHop, int routeInterface, bool isRouteDefault, const std::optional<Address>& routeDestination, int routeMetric) :
    family(routeFamily), via(routeNextHop), dev(routeInterface), defaultRoute(isRouteDefault), to(routeDestination), metric(routeMetric)
{
    // For onlink routes, ensure the via field is empty
    if (via.has_value() && ((family == AF_INET && via->Addr() == "0.0.0.0") || (family == AF_INET6 && via->Addr() == "::")))
    {
        via.reset();
    }
}

std::ostream& operator<<(std::ostream& out, const Route& route)
{
    if (route.defaultRoute)
    {
        out << "default ";
    }

    if (route.to.has_value())
    {
        out << route.to.value() << " ";
    }

    if (route.via.has_value())
    {
        out << " via " << route.via.value() << " ";
    }

    return out << "dev " << route.dev << " metric " << route.metric;
}

bool Route::IsOnlink() const
{
    return !via.has_value();
}

bool Route::IsMulticast() const
{
    if (!to.has_value())
    {
        return false;
    }

    if (family == AF_INET)
    {
        in_addr address = {};
        Syscall(::inet_pton, to->Family(), to->Addr().c_str(), &address);
        return IN_MULTICAST(ntohl(address.s_addr));
    }

    in6_addr address = {};
    Syscall(::inet_pton, to->Family(), to->Addr().c_str(), &address);
    return IN6_IS_ADDR_MULTICAST(&address);
}
