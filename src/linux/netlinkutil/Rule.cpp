// Copyright (C) Microsoft Corporation. All rights reserved.
#include <iostream>
#include "Rule.h"

Rule::Rule(int family, int routingTable, int priority) : family(family), routingTable(routingTable), priority(priority)
{
}

Rule::Rule(int family, int routingTable, int priority, const std::optional<Protocol> protocol) :
    family(family), routingTable(routingTable), priority(priority), protocol(protocol)
{
}

Rule::Rule(int family, int routingTable, int priority, const std::string& oif, const std::optional<Protocol> protocol) :
    family(family), routingTable(routingTable), priority(priority), oif(oif), protocol(protocol)
{
}

std::ostream& operator<<(std::ostream& out, const Rule& rule)
{
    out << "priority " << rule.priority << " family " << (rule.family == AF_INET ? "ipv4" : "ipv6") << " ";

    if (rule.sourceAddress)
    {
        out << "from " << rule.sourceAddress.value() << " ";
    }

    if (rule.protocol)
    {
        out << "ipproto " << (rule.protocol.value() == Protocol::Tcp ? "tcp" : "udp") << " ";
    }

    if (!rule.oif.empty())
    {
        out << "oif " << rule.oif << " ";
    }

    if (!rule.iif.empty())
    {
        out << "iif " << rule.iif << " ";
    }

    return out << "lookup table " << rule.routingTable << " ";
}
