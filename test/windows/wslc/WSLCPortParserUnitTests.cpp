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
        VerifyNoHostPort(result);
        VerifyContainerPort(result, 80, 80);
        VerifyProtocol(result, PublishPort::Protocol::TCP);
    }

    TEST_METHOD(PortParserTest_ContainerPort_WithProtocol_NoIP)
    {
        {
            auto result = PublishPort::Parse("80/tcp");

            VerifyParseState(result, "80/tcp", true, false);
            VerifyNoHostIP(result);
            VerifyNoHostPort(result);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("54/udp");

            VerifyParseState(result, "54/udp", true, false);
            VerifyNoHostIP(result);
            VerifyNoHostPort(result);
            VerifyContainerPort(result, 54, 54);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }
    }

    TEST_METHOD(PortParserTest_IPv4Mappings_Valid)
    {
        {
            auto result = PublishPort::Parse("127.0.0.1:8080:80");

            VerifyParseState(result, "127.0.0.1:8080:80", false, false);
            VerifyHostIPv4(result, "127.0.0.1", true, false, {127, 0, 0, 1});
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("0.0.0.0:8080:80");

            VerifyParseState(result, "0.0.0.0:8080:80", false, false);
            VerifyHostIPv4(result, "0.0.0.0", false, true, {0, 0, 0, 0});
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("192.168.1.50:8080:80");

            VerifyParseState(result, "192.168.1.50:8080:80", false, false);
            VerifyHostIPv4(result, "192.168.1.50", false, false, {192, 168, 1, 50});
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("127.0.0.1:5353:5353/udp");

            VerifyParseState(result, "127.0.0.1:5353:5353/udp", false, false);
            VerifyHostIPv4(result, "127.0.0.1", true, false, {127, 0, 0, 1});
            VerifyHostPort(result, 5353, 5353);
            VerifyContainerPort(result, 5353, 5353);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }

        {
            auto result = PublishPort::Parse("127.0.0.1::80");

            VerifyParseState(result, "127.0.0.1::80", true, false);
            VerifyHostIPv4(result, "127.0.0.1", true, false, {127, 0, 0, 1});
            VerifyNoHostPort(result);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("127.0.0.1::53/udp");

            VerifyParseState(result, "127.0.0.1::53/udp", true, false);
            VerifyHostIPv4(result, "127.0.0.1", true, false, {127, 0, 0, 1});
            VerifyNoHostPort(result);
            VerifyContainerPort(result, 53, 53);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }
    }

    TEST_METHOD(PortParserTest_IPv6Mappings_Valid)
    {
        {
            auto result = PublishPort::Parse("[::1]:8080:80");

            VerifyParseState(result, "[::1]:8080:80", false, false);
            VerifyHostIPv6(result, "::1", true, false, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::]:8080:80");

            VerifyParseState(result, "[::]:8080:80", false, false);
            VerifyHostIPv6(result, "::", false, true, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[2001:db8::10]:8080:80");

            VerifyParseState(result, "[2001:db8::10]:8080:80", false, false);
            VerifyHostIPv6(result, "2001:db8::10", false, false, {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10});
            VerifyHostPort(result, 8080, 8080);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::1]:5353:5353/udp");

            VerifyParseState(result, "[::1]:5353:5353/udp", false, false);
            VerifyHostIPv6(result, "::1", true, false, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
            VerifyHostPort(result, 5353, 5353);
            VerifyContainerPort(result, 5353, 5353);
            VerifyProtocol(result, PublishPort::Protocol::UDP);
        }

        {
            auto result = PublishPort::Parse("[::1]::80");

            VerifyParseState(result, "[::1]::80", true, false);
            VerifyHostIPv6(result, "::1", true, false, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
            VerifyNoHostPort(result);
            VerifyContainerPort(result, 80, 80);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::]::53/udp");

            VerifyParseState(result, "[::]::53/udp", true, false);
            VerifyHostIPv6(result, "::", false, true, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            VerifyNoHostPort(result);
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
            VerifyHostIPv4(result, "127.0.0.1", true, false, {127, 0, 0, 1});
            VerifyHostPort(result, 9000, 9003);
            VerifyContainerPort(result, 9000, 9003);
            VerifyProtocol(result, PublishPort::Protocol::TCP);
        }

        {
            auto result = PublishPort::Parse("[::1]:7000-7002:7000-7002");

            VerifyParseState(result, "[::1]:7000-7002:7000-7002", false, true);
            VerifyHostIPv6(result, "::1", true, false, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
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
        VERIFY_ARE_EQUAL(hasEphemeralHostPort, result.HasEphemeralHostPort());
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

    static void VerifyNoHostPort(const PublishPort& result)
    {
        VERIFY_IS_FALSE(result.HostPort().has_value());
    }

    static void VerifyHostPort(const PublishPort& result, int start, int end)
    {
        VERIFY_IS_TRUE(result.HostPort().has_value());
        VerifyRange(*result.HostPort(), start, end);
    }

    static void VerifyContainerPort(const PublishPort& result, int start, int end)
    {
        VerifyRange(result.ContainerPort(), start, end);
    }

    static void VerifyHostIPv4(
        const PublishPort& result, const std::string& expectedString, bool isLoopback, bool isAllInterfaces, const std::array<uint8_t, 4>& expectedBytes)
    {
        VERIFY_IS_TRUE(result.HostIP().has_value());
        VERIFY_IS_FALSE(result.HostIP()->IsIPv6());
        VERIFY_ARE_EQUAL(isLoopback, result.HostIP()->IsLoopback());
        VERIFY_ARE_EQUAL(isAllInterfaces, result.HostIP()->IsAllInterfaces());
        VERIFY_ARE_EQUAL(expectedString, result.HostIP()->ToString());

        auto bytes = result.HostIP()->GetBytes();
        VERIFY_ARE_EQUAL(expectedBytes[0], bytes[0]);
        VERIFY_ARE_EQUAL(expectedBytes[1], bytes[1]);
        VERIFY_ARE_EQUAL(expectedBytes[2], bytes[2]);
        VERIFY_ARE_EQUAL(expectedBytes[3], bytes[3]);
    }

    static void VerifyHostIPv6(
        const PublishPort& result, const std::string& expectedString, bool isLoopback, bool isAllInterfaces, const std::array<uint8_t, 16>& expectedBytes)
    {
        VERIFY_IS_TRUE(result.HostIP().has_value());
        VERIFY_IS_TRUE(result.HostIP()->IsIPv6());
        VERIFY_ARE_EQUAL(isLoopback, result.HostIP()->IsLoopback());
        VERIFY_ARE_EQUAL(isAllInterfaces, result.HostIP()->IsAllInterfaces());
        VERIFY_ARE_EQUAL(expectedString, result.HostIP()->ToString());

        auto bytes = result.HostIP()->GetBytes();
        for (size_t index = 0; index < expectedBytes.size(); ++index)
        {
            VERIFY_ARE_EQUAL(expectedBytes[index], bytes[index]);
        }
    }
};
} // namespace WSLCPortParserUnitTests
