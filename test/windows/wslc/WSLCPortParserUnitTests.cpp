/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCPortParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC port argument parsing and validation.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include <ContainerModel.h>

namespace WSLCPortParserUnitTests {

using namespace wsl::windows::wslc::models;

class WSLCPortParserUnitTests
{
    WSL_TEST_CLASS(WSLCPortParserUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(PortParserTest_HostAndContainerPort_Valid)
    {
        auto result = PublishPort::Parse("8080:80");

        VerifyParseState(result, "8080:80", false, false);
        VerifyNoHostIP(result);
        VerifyHostPort(result, 8080, 8080);
        VerifyContainerPort(result, 80, 80);
        VerifyProtocol(result, PublishPort::Protocol::TCP);
    }

    TEST_METHOD(PortParserTest_ContainerPort_Only_Valid)
    {
        auto result = PublishPort::Parse("80");

        VerifyParseState(result, "80", true, false);
        VerifyNoHostIP(result);
        VerifyEphemeralHostPort(result);
        VerifyContainerPort(result, 80, 80);
        VerifyProtocol(result, PublishPort::Protocol::TCP);
    }

    TEST_METHOD(PortParserTest_ContainerPort_WithProtocol_NoIP)
    {
        {
            auto result = PublishPort::Parse("80/tcp");

            VerifyParseState(result, "80/tcp", true, false);
            VerifyNoHostIP(result);
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("54/udp");

            VerifyParseState(result, "54/udp", true, false);
            VerifyNoHostIP(result);
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 54, 54);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }
    }

    TEST_METHOD(PortParserTest_ContainerPortRange_Only_Valid)
    {
        auto result = PublishPort::Parse("8000-8005");

        VerifyParseState(result, "8000-8005", true, true);
        VerifyNoHostIP(result);
        VerifyEphemeralHostPort(result);
        VerifyContainerPort(result, 8000, 8005);
        VerifyProtocol(result, PublishPort::Protocol::TCP);
    }

    TEST_METHOD(PortParserTest_ContainerPortRange_WithProtocol_NoIP)
    {
        auto result = PublishPort::Parse("8000-8005/udp");

        VerifyParseState(result, "8000-8005/udp", true, true);
        VerifyNoHostIP(result);
        VerifyEphemeralHostPort(result);
        VerifyContainerPort(result, 8000, 8005);
        VerifyProtocol(result, PublishPort::Protocol::UDP);
    }

    TEST_METHOD(PortParserTest_IPv4Mappings_Valid)
    {
        {
            auto result = PublishPort::Parse("127.0.0.1:8080:80");

            VerifyParseState(result, "127.0.0.1:8080:80", false, false);
            VerifyHostIPv4(result, "127.0.0.1");
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("0.0.0.0:8080:80");

            VerifyParseState(result, "0.0.0.0:8080:80", false, false);
            VerifyHostIPv4(result, "0.0.0.0");
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("192.168.1.50:8080:80");

            VerifyParseState(result, "192.168.1.50:8080:80", false, false);
            VerifyHostIPv4(result, "192.168.1.50");
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("127.0.0.1:5353:5353/udp");

            VerifyParseState(result, "127.0.0.1:5353:5353/udp", false, false);
            VerifyHostIPv4(result, "127.0.0.1");
            VerifyHostPort(result, 5353, 5353);
            VerifyContainerPort(result, 5353, 5353);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }

        {
            auto result = PublishPort::Parse("127.0.0.1::80");

            VerifyParseState(result, "127.0.0.1::80", true, false);
            VerifyHostIPv4(result, "127.0.0.1");
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("127.0.0.1::53/udp");

            VerifyParseState(result, "127.0.0.1::53/udp", true, false);
            VerifyHostIPv4(result, "127.0.0.1");
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 53, 53);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }
    }

    TEST_METHOD(PortParserTest_IPv6Mappings_Valid)
    {
        {
            auto result = PublishPort::Parse("[::1]:8080:80");

            VerifyParseState(result, "[::1]:8080:80", false, false);
            VerifyHostIPv6(result, "::1");
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::]:8080:80");

            VerifyParseState(result, "[::]:8080:80", false, false);
            VerifyHostIPv6(result, "::");
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[2001:db8::10]:8080:80");

            VerifyParseState(result, "[2001:db8::10]:8080:80", false, false);
            VerifyHostIPv6(result, "2001:db8::10");
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::1]:5353:5353/udp");

            VerifyParseState(result, "[::1]:5353:5353/udp", false, false);
            VerifyHostIPv6(result, "::1");
            VerifyHostPort(result, 5353, 5353);
            VerifyContainerPort(result, 5353, 5353);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }

        {
            auto result = PublishPort::Parse("[::1]::80");

            VerifyParseState(result, "[::1]::80", true, false);
            VerifyHostIPv6(result, "::1");
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::]::53/udp");

            VerifyParseState(result, "[::]::53/udp", true, false);
            VerifyHostIPv6(result, "::");
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 53, 53);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }
    }

    TEST_METHOD(PortParserTest_PortRangeMappings_Valid)
    {
        {
            auto result = PublishPort::Parse("8000-8005:8000-8005");

            VerifyParseState(result, "8000-8005:8000-8005", false, true);
            VerifyNoHostIP(result);
            VerifyHostPort(result, 8000, 8005);
            VerifyContainerPort(result, 8000, 8005);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("127.0.0.1:9000-9003:9000-9003");

            VerifyParseState(result, "127.0.0.1:9000-9003:9000-9003", false, true);
            VerifyHostIPv4(result, "127.0.0.1");
            VerifyHostPort(result, 9000, 9003);
            VerifyContainerPort(result, 9000, 9003);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::1]:7000-7002:7000-7002");

            VerifyParseState(result, "[::1]:7000-7002:7000-7002", false, true);
            VerifyHostIPv6(result, "::1");
            VerifyHostPort(result, 7000, 7002);
            VerifyContainerPort(result, 7000, 7002);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("10000-10010:10000-10010/tcp");

            VerifyParseState(result, "10000-10010:10000-10010/tcp", false, true);
            VerifyNoHostIP(result);
            VerifyHostPort(result, 10000, 10010);
            VerifyContainerPort(result, 10000, 10010);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("20000-20002:20000-20002/udp");

            VerifyParseState(result, "20000-20002:20000-20002/udp", false, true);
            VerifyNoHostIP(result);
            VerifyHostPort(result, 20000, 20002);
            VerifyContainerPort(result, 20000, 20002);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }
    }

    TEST_METHOD(PortParserTest_EphemeralHostPort_WithRange_AndIP_Valid)
    {
        {
            auto result = PublishPort::Parse("127.0.0.1::8000-8005");

            VerifyParseState(result, "127.0.0.1::8000-8005", true, true);
            VerifyHostIPv4(result, "127.0.0.1");
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 8000, 8005);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::1]::8000-8005");

            VerifyParseState(result, "[::1]::8000-8005", true, true);
            VerifyHostIPv6(result, "::1");
            VerifyEphemeralHostPort(result);
            VerifyContainerPort(result, 8000, 8005);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }
    }

    TEST_METHOD(PortParserTest_InvalidMappings)
    {
        static const std::vector<std::string> invalidCases = {
            "",                    // Empty input
            " ",                   // Whitespace only
            "80 ",                 // Trailing whitespace
            ":80",                 // Empty host port
            "127.0.0.1:80",        // Missing container port
            "[::1]:8080",          // Missing container port
            "8000-8005:8000-8006", // Mismatched port ranges
            "8000-8005:8000",      // Mismatched port ranges
            "8000:8000-8005",      // Mismatched port ranges
            "8080:80/icmp",        // Invalid protocol
            "8080:80/udpp",        // Invalid protocol
            "80/TCP",              // Protocol is case sensitive
            "80/tcp:90",           // Protocol suffix must be final
            "80-",                 // Malformed port range
            "-80",                 // Malformed port range
            "80--81",              // Malformed port range
            "8000-7000",           // Invalid port range
            "0",                   // Invalid port number
            "65536",               // Invalid port number
        };

        for (const auto& value : invalidCases)
        {
            VERIFY_THROWS(PublishPort::Parse(value), wil::ResultException);
        }
    }

private:
    static void VerifyRange(const PublishPort::PortRange& range, int start, int end)
    {
        VERIFY_ARE_EQUAL(start, range.Start());
        VERIFY_ARE_EQUAL(end, range.End());
        VERIFY_ARE_EQUAL(start == end, range.IsSingle());
    }

    static void VerifyParseState(const PublishPort& result, const std::string& original, bool hasEphemeralHostPort, bool isRangeMapping)
    {
        VERIFY_ARE_EQUAL(original, result.Original());
        VERIFY_ARE_EQUAL(hasEphemeralHostPort, result.HostPort().IsEphemeral());
        VERIFY_ARE_EQUAL(isRangeMapping, result.IsRangeMapping());
    }

    static void VerifyProtocol(const PublishPort& result, PublishPort::Protocol protocol)
    {
        VERIFY_ARE_EQUAL(static_cast<int>(protocol), static_cast<int>(result.PortProtocol()));
    }

    static void VerifyNoHostIP(const PublishPort& result)
    {
        VERIFY_IS_FALSE(result.HostIP().has_value());
    }

    static void VerifyEphemeralHostPort(const PublishPort& result)
    {
        VERIFY_IS_TRUE(result.HostPort().IsEphemeral());
    }

    static void VerifyHostPort(const PublishPort& result, int start, int end)
    {
        VerifyRange(result.HostPort(), start, end);
    }

    static void VerifyContainerPort(const PublishPort& result, int start, int end)
    {
        VerifyRange(result.ContainerPort(), start, end);
    }

    static void VerifyHostIPv4(const PublishPort& result, const std::string& expectedString)
    {
        VERIFY_IS_TRUE(result.HostIP().has_value());
        VERIFY_IS_FALSE(result.HostIP()->IsIPv6());
        VERIFY_ARE_EQUAL(expectedString, result.HostIP()->IP());
    }

    static void VerifyHostIPv6(const PublishPort& result, const std::string& expectedString)
    {
        VERIFY_IS_TRUE(result.HostIP().has_value());
        VERIFY_IS_TRUE(result.HostIP()->IsIPv6());
        VERIFY_ARE_EQUAL(expectedString, result.HostIP()->IP());
    }
};
} // namespace WSLCPortParserUnitTests
