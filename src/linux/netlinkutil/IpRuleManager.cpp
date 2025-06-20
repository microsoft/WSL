// Copyright (C) Microsoft Corporation. All rights reserved.
#include "RuntimeErrorWithSourceLocation.h"
#include "IpRuleManager.h"
#include "NetlinkTransactionError.h"
#include "Utils.h"
#include "common.h"

struct LoopbackInterfaceAttribute
{
    rtattr header;
    // A buffer of size 4 is used for alignment purposes (aligning to NLA_ALIGNTO).
    // The loopback interface name is hardcoded as 'lo'.
    char interface[4] = {'l', 'o', '\0', '\0'};
} __attribute__((packed));

template <DerivedRuleMessage TMessage>
void IpRuleManager::SendMessage(unsigned char family, int routingTable, int operation, int flags, const std::function<void(TMessage&)>& routine)
{
    TMessage message{};
    message.rule.rtm_family = family;
    message.rule.rtm_table = 0;
    message.rule.rtm_protocol = RTPROT_BOOT;
    message.rule.rtm_type = RTN_UNICAST;
    message.rule.rtm_scope = RT_SCOPE_UNIVERSE;

    utils::InitializeIntegerAttribute(message.tableId, routingTable, FRA_TABLE);

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
            // Errors "file exists" and "file not found" are ignored in order to avoid keeping
            // track in GnsDaemon of what rules were added/deleted and allow the same rule
            // to be added/deleted multiple times.
            if (errorCode.value() == -EEXIST || errorCode.value() == -ENOENT)
            {
                return;
            }
        }

        throw;
    }
}

void IpRuleManager::ModifyLoopbackRule(const Rule& rule, Operation operation)
{
    if (!rule.protocol.has_value())
    {
        throw RuntimeErrorWithSourceLocation("Loopback rule missing protocol");
    }
    if (operation != Operation::Create && operation != Operation::Remove)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected operation: {}", static_cast<int>(operation)));
    }
    if (rule.iif.empty())
    {
        throw RuntimeErrorWithSourceLocation("Loopback rule has empty iif name");
    }

    GNS_LOG_INFO("{} rule {}", operation == Operation::Create ? "Add" : "Remove", utils::Stringify(rule).c_str());

    struct Message
    {
        rtmsg rule;
        utils::IntegerAttribute tableId;
        utils::IntegerAttribute priority;
        utils::Attribute<uint8_t> protocol;
        utils::Attribute<char> iifName;
    } __attribute__((packed));

    // In case of Remove, there are no additional flags needed besides NLM_F_REQUEST | NLM_F_ACK.
    int flags = 0;
    if (operation == Create)
    {
        flags = NLM_F_CREATE;
    }

    int netlinkOperation = operation == Remove ? RTM_DELRULE : RTM_NEWRULE;

    auto buffer = std::vector<char>(RTA_ALIGN(offsetof(Message, iifName.value) + rule.iif.size()));
    auto* message = gslhelpers::get_struct<Message>(gsl::make_span(buffer));

    message->rule.rtm_family = rule.family;
    message->rule.rtm_table = 0;
    message->rule.rtm_protocol = RTPROT_BOOT;
    message->rule.rtm_type = RTN_UNICAST;
    message->rule.rtm_scope = RT_SCOPE_UNIVERSE;

    utils::InitializeIntegerAttribute(message->tableId, rule.routingTable, FRA_TABLE);
    utils::InitializeIntegerAttribute(message->priority, rule.priority, RTA_PRIORITY);

    message->protocol.header.rta_len = RTA_LENGTH(sizeof(uint8_t));
    message->protocol.header.rta_type = FRA_IP_PROTO;
    message->protocol.value = rule.protocol.value() == Tcp ? IPPROTO_TCP : IPPROTO_UDP;

    message->iifName.header.rta_len = RTA_SPACE(rule.iif.size());
    message->iifName.header.rta_type = FRA_IIFNAME;
    auto iifNameBuffer = gsl::make_span(buffer).subspan(offsetof(Message, iifName.value));
    gsl::copy(gsl::make_span(rule.iif), iifNameBuffer);

    // The SendMessage helper cannot be used here because we are sending a variable sized message
    // (the message contains the iifName field with variable length)
    auto transaction = m_channel.CreateTransaction(buffer.data(), buffer.size(), netlinkOperation, flags);
    try
    {
        transaction.Execute();
    }
    catch (const NetlinkTransactionError& transactionErr)
    {
        auto errorCode = transactionErr.Error();
        if (errorCode.has_value())
        {
            // Errors "file exists" and "file not found" are ignored in order to avoid keeping
            // track in GnsDaemon of what rules were added/deleted and allow the same rule
            // to be added/deleted multiple times.
            if (errorCode.value() == -EEXIST || errorCode.value() == -ENOENT)
            {
                return;
            }
        }

        throw;
    }
}

void IpRuleManager::ModifyLoopbackRuleWithSourceAddress(const Rule& rule, Operation action)
{
    if (rule.family != AF_INET && rule.family != AF_INET6)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected address family: {}", rule.family));
    }
    if (action != Operation::Create && action != Operation::Remove)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected operation: {}", static_cast<int>(action)));
    }

    if (rule.family == AF_INET)
    {
        ModifyLoopbackRuleWithSourceAddressImpl<in_addr>(rule, action);
    }
    else
    {
        ModifyLoopbackRuleWithSourceAddressImpl<in6_addr>(rule, action);
    }
}

template <typename TAddr>
void IpRuleManager::ModifyLoopbackRuleWithSourceAddressImpl(const Rule& rule, Operation operation)
{
    if (!rule.protocol.has_value())
    {
        throw RuntimeErrorWithSourceLocation("Rule missing protocol");
    }
    if (!rule.sourceAddress.has_value())
    {
        throw RuntimeErrorWithSourceLocation("Rule missing source IP");
    }

    GNS_LOG_INFO("{} rule {}", operation == Operation::Create ? "Add" : "Remove", utils::Stringify(rule).c_str());

    int flags = (operation == Create) ? NLM_F_CREATE : 0;

    struct Message : RuleMessage
    {
        LoopbackInterfaceAttribute devName;
        utils::AddressAttribute<TAddr> from;
        utils::IntegerAttribute priority;
        utils::Attribute<uint8_t> protocol;
    } __attribute__((packed));

    int netlinkOperation = operation == Remove ? RTM_DELRULE : RTM_NEWRULE;

    SendMessage<Message>(rule.family, rule.routingTable, netlinkOperation, flags, [&](Message& message) {
        message.devName.header.rta_len = sizeof(message.devName);
        message.devName.header.rta_type = FRA_IIFNAME;

        // Set source address in the rule
        message.rule.rtm_src_len = rule.sourceAddress->PrefixLength();
        utils::InitializeAddressAttribute<TAddr>(message.from, rule.sourceAddress.value(), FRA_SRC);

        utils::InitializeIntegerAttribute(message.priority, rule.priority, RTA_PRIORITY);

        message.protocol.header.rta_len = RTA_LENGTH(sizeof(uint8_t));
        message.protocol.header.rta_type = FRA_IP_PROTO;
        message.protocol.value = rule.protocol.value() == Tcp ? IPPROTO_TCP : IPPROTO_UDP;
    });
}

void IpRuleManager::ModifyRoutingTablePriority(const Rule& rule, Operation operation)
{
    if (operation != Operation::Create && operation != Operation::Remove)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected operation: {}", static_cast<int>(operation)));
    }

    GNS_LOG_INFO("{} rule {}", operation == Operation::Create ? "Add" : "Remove", utils::Stringify(rule));

    struct Message : RuleMessage
    {
        utils::IntegerAttribute priority;
    } __attribute__((packed));

    // In case of Remove, there are no additional flags needed besides NLM_F_REQUEST | NLM_F_ACK.
    int flags = 0;
    if (operation == Create)
    {
        flags = NLM_F_CREATE;
    }

    int netlinkOperation = operation == Remove ? RTM_DELRULE : RTM_NEWRULE;

    SendMessage<Message>(rule.family, rule.routingTable, netlinkOperation, flags, [&](Message& message) {
        utils::InitializeIntegerAttribute(message.priority, rule.priority, RTA_PRIORITY);
    });
}

void IpRuleManager::ModifyRoutingTablePriorityWithProtocol(const Rule& rule, Operation operation)
{
    if (operation != Operation::Create && operation != Operation::Remove)
    {
        throw RuntimeErrorWithSourceLocation(std::format("Unexpected operation: {}", static_cast<int>(operation)));
    }
    if (!rule.protocol.has_value())
    {
        throw RuntimeErrorWithSourceLocation("Rule missing protocol");
    }

    GNS_LOG_INFO("{} rule {}", operation == Operation::Create ? "Add" : "Remove", utils::Stringify(rule));

    struct Message : RuleMessage
    {
        utils::IntegerAttribute priority;
        utils::Attribute<uint8_t> protocol;
    } __attribute__((packed));

    // In case of Remove, there are no additional flags needed besides NLM_F_REQUEST | NLM_F_ACK.
    int flags = 0;
    if (operation == Create)
    {
        flags = NLM_F_CREATE;
    }

    int netlinkOperation = operation == Remove ? RTM_DELRULE : RTM_NEWRULE;

    SendMessage<Message>(rule.family, rule.routingTable, netlinkOperation, flags, [&](Message& message) {
        utils::InitializeIntegerAttribute(message.priority, rule.priority, RTA_PRIORITY);

        message.protocol.header.rta_len = RTA_LENGTH(sizeof(uint8_t));
        message.protocol.header.rta_type = FRA_IP_PROTO;
        message.protocol.value = rule.protocol.value() == Tcp ? IPPROTO_TCP : IPPROTO_UDP;
    });
}

std::vector<Rule> IpRuleManager::ListRules(int family, int tableId)
{
    struct Message
    {
        rtmsg rule;
    } __attribute__((packed));

    std::vector<Rule> rules{};

    auto onMessage = [&](const NetlinkResponse& response) {
        for (const auto& e : response.Messages<rtmsg>(RTM_NEWRULE))
        {
            const auto msg = e.Payload();
            const auto priorityAttr = e.UniqueAttribute<int>(FRA_PRIORITY);
            const auto oifAttr = e.UniqueAttribute<char>(FRA_OIFNAME);
            const auto protocolAttr = e.UniqueAttribute<uint8_t>(FRA_IP_PROTO);
            const auto tableAttr = e.UniqueAttribute<int>(FRA_TABLE);

            int priority = priorityAttr.has_value() ? *priorityAttr.value() : -1;
            std::string oif(oifAttr.value_or(""));
            uint8_t proto = protocolAttr.has_value() ? *protocolAttr.value() : -1;
            std::optional<Protocol> protocol =
                (proto == IPPROTO_TCP) ? std::optional(Tcp) : ((proto == IPPROTO_UDP) ? std::optional(Udp) : std::optional<Protocol>());
            int tableAttrId = tableAttr.has_value() ? *tableAttr.value() : msg->rtm_table;

            rules.emplace_back(Rule(msg->rtm_family, tableAttrId, priority, oif, protocol));
        }
    };

    rtmsg message{};
    message.rtm_family = family;
    message.rtm_table = 0;

    auto transaction = m_channel.CreateTransaction(message, RTM_GETRULE, NLM_F_DUMP);
    transaction.Execute(onMessage);

    return rules;
}
