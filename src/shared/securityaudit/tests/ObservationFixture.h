// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "SecurityFixture.h"
#include "ObservationSchema.h"

inline nlohmann::json ObservationFixture(std::string_view operation)
{
    using namespace wsl::audit;
    auto event = SecurityFixture(operation);
    auto& fields = event["fields"];
    const auto family = SecurityFamily(operation);
    if (family == "network")
    {
        if (operation == "dns-query")
        {
            fields["dnsName"] = "example.test";
            fields["dnsAddresses"] = {"127.0.0.1", "::1"};
        }
        else
        {
            fields["socketFd"] = 0;
            fields["socketInode"] = UINT64_MAX;
            fields["socketFamily"] = 2U;
            fields["socketType"] = 1U;
            fields["localIp"] = "127.0.0.1";
            fields["localPort"] = 12345U;
            if (operation == "socket")
            {
                fields["requestedProtocol"] = 0;
            }
            else if (operation == "bind")
            {
                fields["requestedIp"] = "0.0.0.0";
                fields["requestedPort"] = 0U;
            }
            else
            {
                fields["remoteIp"] = "127.0.0.1";
                fields["remotePort"] = 80U;
                if (operation == "accept" || operation == "accept4")
                {
                    fields["acceptedFd"] = 0;
                }
            }
        }
    }
    else if (family == "module-load")
    {
        if (operation == "init_module")
        {
            fields["imageSize"] = UINT64_MAX;
        }
        else
        {
            fields["moduleFd"] = 0;
            fields["flags"] = uint64_t{0};
        }
    }
    else if (family == "privilege-transition")
    {
        fields["newUid"] = event["uid"];
        fields["newGid"] = event["gid"];
        if (operation == "setuid")
        {
            fields["requestedUid"] = 0U;
            fields["oldGid"] = event["gid"];
        }
        else
        {
            fields["oldUid"] = event["uid"];
            if (operation == "setgid")
            {
                fields["requestedGid"] = 0U;
            }
            else
            {
                fields["requestedRealGid"] = UINT32_MAX;
                fields["requestedEffectiveGid"] = 0U;
                fields["requestedSavedGid"] = UINT32_MAX;
            }
        }
    }
    else
    {
        fields["requestedFlags"] = uint64_t{0};
        fields["namespaceTypes"] = 0U;
        fields["previousMountNamespace"] = 11U;
        fields["operationMountNamespace"] = 12U;
        if (operation == "setns")
        {
            fields["namespaceFd"] = 0;
        }
    }
    return event;
}
