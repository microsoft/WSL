// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "ProxyBypass.hpp"

using wsl::windows::common::proxy::NormalizeBypass;

namespace ProxyBypassUnitTests {

class ProxyBypassUnitTests
{
    WSL_TEST_CLASS(ProxyBypassUnitTests)

    TEST_METHOD(IPv4PrefixWildcards)
    {
        const std::vector<std::pair<std::string, std::string>> cases{
            {"127.*", "127.0.0.0/8"},
            {"10.*", "10.0.0.0/8"},
            {"10.*.*.*", "10.0.0.0/8"},
            {"192.168.*", "192.168.0.0/16"},
            {"192.168.*.*", "192.168.0.0/16"},
            {"192.168.1.*", "192.168.1.0/24"},
            {"0.*", "0.0.0.0/8"},
            {"255.255.255.*", "255.255.255.0/24"},
            {" \t127.*\r\n", "127.0.0.0/8"}};

        for (const auto& [input, expected] : cases)
        {
            VERIFY_ARE_EQUAL(expected, NormalizeBypass(input));
        }

        for (unsigned int subnet = 16; subnet <= 31; ++subnet)
        {
            VERIFY_ARE_EQUAL(std::format("172.{}.0.0/16", subnet), NormalizeBypass(std::format("172.{}.*", subnet)));
        }
    }

    TEST_METHOD(OtherEntriesArePreserved)
    {
        for (const std::string input :
             {"",
              " \t",
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
              "  *.example.com  "})
        {
            VERIFY_ARE_EQUAL(input, NormalizeBypass(input));
        }
    }

    TEST_METHOD(InvalidIPv4PatternsArePreserved)
    {
        for (const std::string input :
             {"256.*",
              "192.256.*",
              "-1.*",
              "+1.*",
              "01.*",
              "192.168.01.*",
              "999999999999.*",
              "127.*.1",
              "127.*.1.*",
              "127.0.0.1.*",
              "127.*.*.*.*",
              "127..*",
              ".127.*",
              "127.*.",
              "127.0*",
              "127.?.*",
              "127.*:80",
              "127 . *"})
        {
            VERIFY_ARE_EQUAL(input, NormalizeBypass(input));
        }
    }
};

} // namespace ProxyBypassUnitTests
