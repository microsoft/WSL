// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <optional>
#include "address.h"
#include "Protocol.h"

/*
    Currently Rule only supports family, table, priority, input or output interface, source IP and protocol (tcp/udp).
    It can be extended destination IP and source/destination port.
*/
struct Rule
{
    int family = AF_INET;
    int routingTable = -1;
    int priority = -1;
    std::string iif;
    std::string oif;
    std::optional<Protocol> protocol;
    std::optional<Address> sourceAddress;

    Rule(int family, int routingTable, int priority);

    Rule(int family, int routingTable, int priority, std::optional<Protocol> protocol);

    Rule(int family, int routingTable, int priority, const std::string& oif, std::optional<Protocol> protocol);
};

std::ostream& operator<<(std::ostream& out, const Rule& rule);