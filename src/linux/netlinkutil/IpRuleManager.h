// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <functional>
#include "NetlinkChannel.h"
#include "Rule.h"
#include "Operation.h"

struct RuleMessage
{
    rtmsg rule;
    utils::IntegerAttribute tableId;
} __attribute__((packed));

// The below a means to ensure that messages have a common set of fields.  It just means
// that a type T must inherit from RuleMessage, and any functions that reference
// DerivedRuleMessage can be assured that they can safely access the fields in RuleMessage.
template <typename TMessage>
concept DerivedRuleMessage = std::is_base_of<RuleMessage, TMessage>::value;

class IpRuleManager
{
public:
    /*
        Implements netlink equivalent of "ip rule <operation> iif <interface> ipproto <protocol> prio <priority> table <table>".
    */
    void ModifyLoopbackRule(const Rule& rule, Operation operation);

    /*
        Implements netlink equivalent of "ip rule <operation> from <source IP> iif lo ipproto <protocol> prio <priority> table <table>".
    */
    void ModifyLoopbackRuleWithSourceAddress(const Rule& rule, Operation operation);

    /*
        Implements netlink equivalent of "ip rule <operation> ipproto <protocol> prio <priority> table <table>".
    */
    void ModifyRoutingTablePriorityWithProtocol(const Rule& rule, Operation operation);

    /*
        Implements netlink equivalent of "ip rule <operation> prio <priority> table <table>".
    */
    void ModifyRoutingTablePriority(const Rule& rule, Operation operation);

    /*
        Implements netlink equivalent of "ip rule show".
    */
    std::vector<Rule> ListRules(int family = AF_INET, int tableId = RT_TABLE_UNSPEC);

private:
    /*
        Creates the message using the routine, then sends the message via netlink.
    */
    template <DerivedRuleMessage TMessage>
    void SendMessage(unsigned char family, int routingTable, int operation, int flags, const std::function<void(TMessage&)>& routine = [](auto&) {});

    template <typename TAddr>
    void ModifyLoopbackRuleWithSourceAddressImpl(const Rule& rule, Operation operation);

    NetlinkChannel m_channel;
};
