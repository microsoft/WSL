/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::shared::string;

class WSLCE2ENetworkListTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkListTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName);
        EnsureNetworkDoesNotExist(TestNetworkName2);
        for (const auto& name : FilterTestNetworkNames)
        {
            EnsureNetworkDoesNotExist(name);
        }
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName);
        EnsureNetworkDoesNotExist(TestNetworkName2);
        for (const auto& name : FilterTestNetworkNames)
        {
            EnsureNetworkDoesNotExist(name);
        }
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_HelpCommand)
    {
        auto result = RunWslc(L"network list --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_InvalidFormatOption)
    {
        auto result = RunWslc(L"network list --format invalid");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table."));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_QuietOption_OutputsNamesOnly)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"network list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto lines = result.GetStdoutLines();
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestNetworkName));
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestNetworkName2));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_JsonFormat)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"network list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto networks = ParseNdjsonOutputAs<WSLCNetworkInformation>(result);
        VERIFY_ARE_EQUAL(2U, networks.size());

        std::vector<std::string> names;
        names.reserve(networks.size());
        for (const auto& network : networks)
        {
            names.push_back(network.Name);
        }

        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestNetworkName)));
        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestNetworkName2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_Filter_MalformedValue)
    {
        // Filter values must be of the form key=value; bare keys are rejected by the CLI.
        const auto result = RunWslc(L"network list --filter label");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_InvalidFilterError(L"label")));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_Filter_InvalidKey)
    {
        // Unknown filter keys are rejected by the Docker daemon.
        const auto result = RunWslc(L"network list --filter color=blue");
        VERIFY_ARE_EQUAL(1, result.ExitCode);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stderr->find(L"invalid filter"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_Filter_Driver)
    {
        const std::wstring alpha = L"wslc-flt-list-driver-alpha";
        const std::wstring beta = L"wslc-flt-list-driver-beta";
        auto cleanup = wil::scope_exit([&]() {
            EnsureNetworkDoesNotExist(alpha);
            EnsureNetworkDoesNotExist(beta);
        });

        auto result = RunWslc(std::format(L"network create --driver bridge {}", alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", beta));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"network list --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto networks = ParseNdjsonOutputAs<WSLCNetworkInformation>(r);
            std::set<std::string> names;
            for (const auto& n : networks)
            {
                names.insert(n.Name);
            }
            return names;
        };

        {
            const auto names = listNames(L"--filter driver=bridge");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(beta)));
        }

        // overlay networks require swarm mode; none exist in the test session.
        {
            const auto names = listNames(L"--filter driver=overlay");
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_Filter_Label)
    {
        const std::wstring alpha = L"wslc-flt-list-label-alpha";
        const std::wstring beta = L"wslc-flt-list-label-beta";
        const std::wstring scopeKey = L"wslc.e2e.list_filter_label";
        const std::wstring scopeValue = L"1";

        auto cleanup = wil::scope_exit([&]() {
            EnsureNetworkDoesNotExist(alpha);
            EnsureNetworkDoesNotExist(beta);
        });

        // alpha carries both scope-key=1 and env=prod; beta carries only scope-key=1.
        auto result =
            RunWslc(std::format(L"network create --driver bridge --label {}={} --label env=prod {}", scopeKey, scopeValue, alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network create --driver bridge --label {}={} {}", scopeKey, scopeValue, beta));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"network list --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto networks = ParseNdjsonOutputAs<WSLCNetworkInformation>(r);
            std::set<std::string> names;
            for (const auto& n : networks)
            {
                names.insert(n.Name);
            }
            return names;
        };

        // label=<key> (key-only) matches any value.
        {
            const auto names = listNames(std::format(L"--filter label={}", scopeKey));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(beta)));
        }

        // label=<key>=<value> narrows to alpha.
        {
            const auto names = listNames(std::format(L"--filter label={}={} --filter label=env=prod", scopeKey, scopeValue));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }

        // Multiple --filter label= entries are AND'd.
        {
            const auto names = listNames(std::format(L"--filter label={} --filter label=env=prod", scopeKey));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_Filter_JsonEmptyIsExactlyEmpty)
    {
        const std::wstring alpha = L"wslc-flt-list-empty-alpha";
        auto cleanup = wil::scope_exit([&]() { EnsureNetworkDoesNotExist(alpha); });

        auto result = RunWslc(std::format(L"network create --driver bridge {}", alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // NDJSON with zero rows must be exactly empty stdout — not "[]", not "\n".
        result = RunWslc(L"network list --format json --filter name=wslc-flt-list-no-such-network-zzz");
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_Filter_Name)
    {
        const std::wstring alpha = L"wslc-flt-list-name-alpha";
        const std::wstring beta = L"wslc-flt-list-name-beta";
        auto cleanup = wil::scope_exit([&]() {
            EnsureNetworkDoesNotExist(alpha);
            EnsureNetworkDoesNotExist(beta);
        });

        auto result = RunWslc(std::format(L"network create --driver bridge {}", alpha));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", beta));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"network list --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto networks = ParseNdjsonOutputAs<WSLCNetworkInformation>(r);
            std::set<std::string> names;
            for (const auto& n : networks)
            {
                names.insert(n.Name);
            }
            return names;
        };

        // Docker's `name` filter is a substring match; the shared prefix picks up both networks.
        {
            const auto names = listNames(L"--filter name=wslc-flt-list-name-");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(beta)));
        }

        // A narrower substring selects only the matching one.
        {
            const auto names = listNames(L"--filter name=name-alpha");
            VERIFY_IS_TRUE(names.contains(WideToMultiByte(alpha)));
            VERIFY_IS_FALSE(names.contains(WideToMultiByte(beta)));
        }
    }

private:
    const std::wstring TestNetworkName = L"wslc-e2e-network-list";
    const std::wstring TestNetworkName2 = L"wslc-e2e-network-list-2";
    const std::vector<std::wstring> FilterTestNetworkNames = {
        L"wslc-flt-list-driver-alpha",
        L"wslc-flt-list-driver-beta",
        L"wslc-flt-list-label-alpha",
        L"wslc-flt-list-label-beta",
        L"wslc-flt-list-empty-alpha",
        L"wslc-flt-list-name-alpha",
        L"wslc-flt-list-name-beta",
    };
};
} // namespace WSLCE2ETests
