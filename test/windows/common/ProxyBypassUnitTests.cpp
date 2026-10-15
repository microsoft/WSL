// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "ProxyBypass.hpp"

using wsl::windows::common::proxy::Ipv4WildcardToCidr;

namespace ProxyBypassUnitTests {

class ProxyBypassUnitTests
{
    WSL_TEST_CLASS(ProxyBypassUnitTests)

    TEST_METHOD(IPv4PrefixWildcards)
    {
        const std::vector<std::pair<std::string, std::string>> cases{
            {"127.*", "127.0.0.0/8"},
            {"127.*.*", "127.0.0.0/8"},
            {"10.*", "10.0.0.0/8"},
            {"10.*.*.*", "10.0.0.0/8"},
            {"192.168.*", "192.168.0.0/16"},
            {"192.168.*.*", "192.168.0.0/16"},
            {"192.168.1.*", "192.168.1.0/24"},
            {"0.*", "0.0.0.0/8"},
            {"255.255.255.*", "255.255.255.0/24"},
            {"001.002.*", "001.002.0.0/16"}};

        for (const auto& [input, expected] : cases)
        {
            VERIFY_ARE_EQUAL(expected, Ipv4WildcardToCidr(input));
        }

        for (unsigned int subnet = 16; subnet <= 31; ++subnet)
        {
            VERIFY_ARE_EQUAL(std::format("172.{}.0.0/16", subnet), Ipv4WildcardToCidr(std::format("172.{}.*", subnet)));
        }
    }

    TEST_METHOD(OtherEntriesArePreserved)
    {
        for (const std::string input :
             {"",
              "localhost",
              "127.0.0.1",
              "::1",
              "127.0.0.0/8",
              "example.com",
              ".example.com",
              "*.example.com",
              "127.example.*",
              "127.*.example.com",
              "*",
              "*.*",
              "<local>",
              "<-loopback>",
              "*.example.com:443"})
        {
            VERIFY_ARE_EQUAL(input, Ipv4WildcardToCidr(input));
        }
    }
};

} // namespace ProxyBypassUnitTests
