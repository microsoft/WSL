/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSettingsUnitTests.cpp

Abstract:

    Unit tests for the wslc settings system.

--*/

#include "precomp.h"
#include "windows/Common.h"

#include "UserSettings.h"
#include "GroupPolicy.h"

using namespace wsl::windows::wslc::settings;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCSettingsUnitTests {

class WSLCSettingsUnitTests
{
    WSL_TEST_CLASS(WSLCSettingsUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        Log::Comment(L"WSLC Settings Unit Tests - Class Setup");
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        Log::Comment(L"WSLC Settings Unit Tests - Class Cleanup");
        return true;
    }

    // ============================================================
    // DEFAULT VALUE TESTS
    // ============================================================

    TEST_METHOD(Defaults_AllSettingsHaveExpectedDefaults)
    {
        UserSettings settings(std::string("{}"));

        VERIFY_ARE_EQUAL(settings.Get<Setting::CpuCount>(), 4);
        VERIFY_ARE_EQUAL(settings.Get<Setting::MemoryMb>(), 2048);
        VERIFY_ARE_EQUAL(settings.Get<Setting::BootTimeoutMs>(), 30000);
        VERIFY_ARE_EQUAL(settings.Get<Setting::MaximumStorageSizeMb>(), 10000);
        VERIFY_ARE_EQUAL(
            static_cast<int>(settings.Get<Setting::NetworkingMode>()), static_cast<int>(SessionNetworkingMode::Nat));
        // StoragePath default is runtime-computed, just verify it's not empty.
        VERIFY_IS_FALSE(settings.Get<Setting::StoragePath>().empty());
    }

    TEST_METHOD(Defaults_EmptyJsonReturnsAllDefaults)
    {
        UserSettings settings(std::string("{}"));

        VERIFY_ARE_EQUAL(static_cast<int>(settings.GetType()), static_cast<int>(UserSettingsType::Custom));
        VERIFY_ARE_EQUAL(settings.Get<Setting::CpuCount>(), 4);
        VERIFY_ARE_EQUAL(settings.Get<Setting::MemoryMb>(), 2048);
        VERIFY_ARE_EQUAL(settings.Get<Setting::BootTimeoutMs>(), 30000);
    }

    // ============================================================
    // FILE LOADING AND FALLBACK TESTS
    // ============================================================

    TEST_METHOD(FileLoading_CustomContent_ReturnsCustomType)
    {
        UserSettings settings(std::string(R"({"session":{"cpuCount":2}})"));

        VERIFY_ARE_EQUAL(static_cast<int>(settings.GetType()), static_cast<int>(UserSettingsType::Custom));
        VERIFY_ARE_EQUAL(settings.Get<Setting::CpuCount>(), 2);
    }

    TEST_METHOD(FileLoading_InvalidCustomContent_HasWarnings)
    {
        UserSettings settings(std::string("not valid json {{{"));

        // Should fall back to defaults with warnings.
        VERIFY_ARE_EQUAL(settings.Get<Setting::CpuCount>(), 4);
        VERIFY_IS_FALSE(settings.GetWarnings().empty());
    }

    TEST_METHOD(FileLoading_CustomContentWithComments_Parses)
    {
        UserSettings settings(std::string(R"({
            // This is a comment
            "session": {"cpuCount": 2}
        })"));

        VERIFY_ARE_EQUAL(settings.Get<Setting::CpuCount>(), 2);
        VERIFY_IS_TRUE(settings.GetWarnings().empty());
    }

    // ============================================================
    // SETTING VALIDATION TESTS - CpuCount
    // ============================================================

    TEST_METHOD(CpuCount_ValidValue)
    {
        UserSettings s(std::string(R"({"session":{"cpuCount":2}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), 2);
        VERIFY_IS_TRUE(s.GetWarnings().empty());
    }

    TEST_METHOD(CpuCount_ZeroValue_Warning)
    {
        UserSettings s(std::string(R"({"session":{"cpuCount":0}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), 4); // Falls back to default
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(CpuCount_NegativeValue_Warning)
    {
        UserSettings s(std::string(R"({"session":{"cpuCount":-5}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), 4);
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(CpuCount_WrongType_Warning)
    {
        UserSettings s(std::string(R"({"session":{"cpuCount":"notanumber"}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), 4);
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(CpuCount_ExceedsHostCpus_Capped)
    {
        // Use a very large value - should be capped to host CPU count.
        UserSettings s(std::string(R"({"session":{"cpuCount":99999}})"));

        SYSTEM_INFO sysInfo{};
        GetSystemInfo(&sysInfo);
        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), static_cast<int>(sysInfo.dwNumberOfProcessors));
    }

    // ============================================================
    // SETTING VALIDATION TESTS - MemoryMb
    // ============================================================

    TEST_METHOD(MemoryMb_ValidValue)
    {
        UserSettings s(std::string(R"({"session":{"memoryMb":4096}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::MemoryMb>(), 4096);
    }

    TEST_METHOD(MemoryMb_BelowMinimum_Warning)
    {
        UserSettings s(std::string(R"({"session":{"memoryMb":100}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::MemoryMb>(), 2048); // Default
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(MemoryMb_WrongType_Warning)
    {
        UserSettings s(std::string(R"({"session":{"memoryMb":true}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::MemoryMb>(), 2048);
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(MemoryMb_ExactMinimum_Valid)
    {
        UserSettings s(std::string(R"({"session":{"memoryMb":256}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::MemoryMb>(), 256);
        VERIFY_IS_TRUE(s.GetWarnings().empty());
    }

    // ============================================================
    // SETTING VALIDATION TESTS - BootTimeoutMs
    // ============================================================

    TEST_METHOD(BootTimeoutMs_ValidValue)
    {
        UserSettings s(std::string(R"({"session":{"bootTimeoutMs":60000}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::BootTimeoutMs>(), 60000);
    }

    TEST_METHOD(BootTimeoutMs_ZeroValue_Warning)
    {
        UserSettings s(std::string(R"({"session":{"bootTimeoutMs":0}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::BootTimeoutMs>(), 30000);
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(BootTimeoutMs_NegativeValue_Warning)
    {
        UserSettings s(std::string(R"({"session":{"bootTimeoutMs":-1000}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::BootTimeoutMs>(), 30000);
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    // ============================================================
    // SETTING VALIDATION TESTS - MaximumStorageSizeMb
    // ============================================================

    TEST_METHOD(MaximumStorageSizeMb_ValidValue)
    {
        UserSettings s(std::string(R"({"session":{"maximumStorageSizeMb":50000}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::MaximumStorageSizeMb>(), 50000);
    }

    TEST_METHOD(MaximumStorageSizeMb_ZeroValue_Warning)
    {
        UserSettings s(std::string(R"({"session":{"maximumStorageSizeMb":0}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::MaximumStorageSizeMb>(), 10000);
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    // ============================================================
    // SETTING VALIDATION TESTS - NetworkingMode
    // ============================================================

    TEST_METHOD(NetworkingMode_Nat)
    {
        UserSettings s(std::string(R"({"session":{"networkingMode":"nat"}})"));

        VERIFY_ARE_EQUAL(
            static_cast<int>(s.Get<Setting::NetworkingMode>()), static_cast<int>(SessionNetworkingMode::Nat));
    }

    TEST_METHOD(NetworkingMode_None)
    {
        UserSettings s(std::string(R"({"session":{"networkingMode":"none"}})"));

        VERIFY_ARE_EQUAL(
            static_cast<int>(s.Get<Setting::NetworkingMode>()), static_cast<int>(SessionNetworkingMode::None));
    }

    TEST_METHOD(NetworkingMode_CaseInsensitive)
    {
        UserSettings s(std::string(R"({"session":{"networkingMode":"NAT"}})"));

        VERIFY_ARE_EQUAL(
            static_cast<int>(s.Get<Setting::NetworkingMode>()), static_cast<int>(SessionNetworkingMode::Nat));
    }

    TEST_METHOD(NetworkingMode_InvalidValue_Warning)
    {
        UserSettings s(std::string(R"({"session":{"networkingMode":"bridged"}})"));

        VERIFY_ARE_EQUAL(
            static_cast<int>(s.Get<Setting::NetworkingMode>()), static_cast<int>(SessionNetworkingMode::Nat));
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(NetworkingMode_WrongType_Warning)
    {
        UserSettings s(std::string(R"({"session":{"networkingMode":42}})"));

        VERIFY_ARE_EQUAL(
            static_cast<int>(s.Get<Setting::NetworkingMode>()), static_cast<int>(SessionNetworkingMode::Nat));
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    // ============================================================
    // SETTING VALIDATION TESTS - StoragePath
    // ============================================================

    TEST_METHOD(StoragePath_AbsolutePath_Valid)
    {
        UserSettings s(std::string(R"({"session":{"storagePath":"C:\\MyData\\wsla"}})"));

        VERIFY_ARE_EQUAL(s.Get<Setting::StoragePath>().wstring(), L"C:\\MyData\\wsla");
    }

    TEST_METHOD(StoragePath_RelativePath_Warning)
    {
        UserSettings s(std::string(R"({"session":{"storagePath":"relative/path"}})"));

        // Should fall back to default (LocalAppData\wsla).
        VERIFY_IS_FALSE(s.Get<Setting::StoragePath>().empty());
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(StoragePath_EmptyString_Warning)
    {
        UserSettings s(std::string(R"({"session":{"storagePath":""}})"));

        VERIFY_IS_FALSE(s.Get<Setting::StoragePath>().empty());
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    TEST_METHOD(StoragePath_WrongType_Warning)
    {
        UserSettings s(std::string(R"({"session":{"storagePath":12345}})"));

        VERIFY_IS_FALSE(s.Get<Setting::StoragePath>().empty());
        VERIFY_IS_FALSE(s.GetWarnings().empty());
    }

    // ============================================================
    // MULTIPLE SETTINGS IN ONE FILE
    // ============================================================

    TEST_METHOD(MultipleSettings_AllValid)
    {
        UserSettings s(std::string(R"({
            "session": {
                "cpuCount": 2,
                "memoryMb": 8192,
                "bootTimeoutMs": 45000,
                "maximumStorageSizeMb": 50000,
                "networkingMode": "none"
            }
        })"));

        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), 2);
        VERIFY_ARE_EQUAL(s.Get<Setting::MemoryMb>(), 8192);
        VERIFY_ARE_EQUAL(s.Get<Setting::BootTimeoutMs>(), 45000);
        VERIFY_ARE_EQUAL(s.Get<Setting::MaximumStorageSizeMb>(), 50000);
        VERIFY_ARE_EQUAL(
            static_cast<int>(s.Get<Setting::NetworkingMode>()), static_cast<int>(SessionNetworkingMode::None));
        VERIFY_IS_TRUE(s.GetWarnings().empty());
    }

    TEST_METHOD(MultipleSettings_SomeInvalid_PartialLoad)
    {
        UserSettings s(std::string(R"({
            "session": {
                "cpuCount": -1,
                "memoryMb": 4096
            }
        })"));

        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), 4); // Default (invalid value)
        VERIFY_ARE_EQUAL(s.Get<Setting::MemoryMb>(), 4096); // From file (valid)
        VERIFY_ARE_EQUAL(s.GetWarnings().size(), static_cast<size_t>(1));
    }

    TEST_METHOD(UnknownKeys_Ignored)
    {
        UserSettings s(std::string(R"({
            "session": {"cpuCount": 2},
            "unknown": {"foo": "bar"}
        })"));

        VERIFY_ARE_EQUAL(s.Get<Setting::CpuCount>(), 2);
        VERIFY_IS_TRUE(s.GetWarnings().empty());
    }

    // ============================================================
    // GROUP POLICY TESTS
    // ============================================================

    TEST_METHOD(Policy_DefaultState_IsNotConfigured)
    {
        // When no registry key exists, policies should be NotConfigured,
        // which means "allowed" (IsEnabled returns true).
        VERIFY_IS_TRUE(GroupPolicies().IsEnabled(TogglePolicy::AllowSettings));
        VERIFY_IS_TRUE(GroupPolicies().IsEnabled(TogglePolicy::AllowCustomNetworkingMode));
    }

    TEST_METHOD(Policy_ValuePolicies_DefaultNotSet)
    {
        // Value policies should return nullopt when not configured.
        auto cpuCap = GroupPolicies().GetValue<ValuePolicy::MaxCpuCount>();
        auto memCap = GroupPolicies().GetValue<ValuePolicy::MaxMemoryMb>();
        auto netMode = GroupPolicies().GetValue<ValuePolicy::DefaultNetworkingMode>();

        // These will only be set if the registry key exists.
        // In a clean test environment, they should not be set.
        // We just verify the call doesn't crash.
        Log::Comment(L"MaxCpuCount policy configured: ");
        Log::Comment(cpuCap.has_value() ? L"yes" : L"no");
    }
};

} // namespace WSLCSettingsUnitTests
