/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLISettingsUnitTests.cpp

Abstract:

    Unit tests for the wslc UserSettings system: SettingsMap, YAML loading,
    per-setting validation, fallback logic, and UserSettingsType detection.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "UserSettings.h"

#include <atomic>
#include <fstream>

using namespace wsl::windows::wslc::settings;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLISettingsUnitTests {

// Thin subclass that makes the protected constructor publicly accessible,
// allowing tests to load settings from an arbitrary directory without
// going through the singleton.
class UserSettingsTest : public UserSettings
{
public:
    explicit UserSettingsTest(const std::filesystem::path& settingsDir) : UserSettings(settingsDir)
    {
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::atomic<int> s_dirCounter{0};

static std::filesystem::path UniqueTempDir()
{
    auto dir = std::filesystem::temp_directory_path() / L"WSLCSettingsTests" / std::to_wstring(GetCurrentProcessId()) /
               std::to_wstring(++s_dirCounter);
    std::filesystem::create_directories(dir);
    return dir;
}

static void WriteFile(const std::filesystem::path& path, std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    VERIFY_IS_TRUE(f.is_open());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// ---------------------------------------------------------------------------
// Test class
// ---------------------------------------------------------------------------

class WSLCCLISettingsUnitTests
{
    WSL_TEST_CLASS(WSLCCLISettingsUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / L"WSLCSettingsTests", ec);
        return true;
    }

    // -----------------------------------------------------------------------
    // SettingsMap — pure unit tests, no I/O
    // -----------------------------------------------------------------------

    // All four settings should return their compile-time defaults when the map
    // is empty (no values have been inserted).
    TEST_METHOD(SettingsMap_GetOrDefault_ReturnsBuiltInWhenAbsent)
    {
        SettingsMap map;
        VERIFY_ARE_EQUAL(4u, map.GetOrDefault<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(2048u, map.GetOrDefault<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(102400u, map.GetOrDefault<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(std::wstring{}, map.GetOrDefault<Setting::SessionStoragePath>());
    }

    // After inserting a value, GetOrDefault must return it rather than the default.
    TEST_METHOD(SettingsMap_GetOrDefault_ReturnsStoredWhenPresent)
    {
        SettingsMap map;
        map.Add<Setting::SessionCpuCount>(16u);
        VERIFY_ARE_EQUAL(16u, map.GetOrDefault<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(2048u, map.GetOrDefault<Setting::SessionMemoryMb>());
    }

    // -----------------------------------------------------------------------
    // Default (setting file missing)
    // -----------------------------------------------------------------------

    // When settings file missing, the type must be Default, there
    // must be no warnings, and all values must be at their built-in defaults.
    TEST_METHOD(LoadSettings_NoFiles_YieldsDefaultTypeAndNoWarnings)
    {
        UserSettingsTest s{UniqueTempDir()};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Default), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(2048u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(102400u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(std::wstring{}, s.Get<Setting::SessionStoragePath>());
    }

    // -----------------------------------------------------------------------
    // Standard (valid settings)
    // -----------------------------------------------------------------------

    // A well-formed settings file must set the type to Standard with no
    // warnings and the specified values loaded.
    TEST_METHOD(LoadSettings_ValidSettings_YieldsStandardTypeAndValues)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: 8\n"
            "  memorySize: 4GB\n"
            "  maxStorageSize: 20000MB\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Standard), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(8u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(4096u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(20000u, s.Get<Setting::SessionStorageSizeMb>());
        // Unspecified setting falls back to built-in default.
        VERIFY_ARE_EQUAL(std::wstring{}, s.Get<Setting::SessionStoragePath>());
    }

    // An empty settings file is valid YAML (null document); all settings use
    // their defaults with no warnings.
    TEST_METHOD(LoadSettings_EmptySettings_AllDefaultsNoWarnings)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(2048u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(102400u, s.Get<Setting::SessionStorageSizeMb>());
    }

    // When the settings file fails to parse, the type is Default and a warning is emitted.
    TEST_METHOD(LoadSettings_InvalidSettings_YieldsDefaultTypeWithWarning)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session: [\n"); // broken YAML (unclosed flow seq)

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Default), static_cast<int>(s.GetType()));
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
    }

    // -----------------------------------------------------------------------
    // Per-setting validation
    // -----------------------------------------------------------------------

    // cpuCount: 0 must be rejected; the default (4) is used and a warning emitted.
    TEST_METHOD(Validation_CpuCount_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  cpuCount: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
        VERIFY_IS_FALSE(s.GetWarnings().front().SettingPath.empty());
    }

    // memorySize: 0 must be rejected; the default (2048) is used.
    TEST_METHOD(Validation_MemoryMb_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  memorySize: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(2048u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
    }

    // maxStorageSize: 0 must be rejected; the default (100GB) is used.
    TEST_METHOD(Validation_StorageSizeMb_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  maxStorageSize: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(102400u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
    }

    // A string where a uint32_t is expected must emit a type warning and fall
    // back to the default.
    TEST_METHOD(Validation_WrongType_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  cpuCount: \"not-a-number\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
    }

    // A valid defaultStoragePath string must survive the UTF-8 → wstring round-trip.
    TEST_METHOD(Validation_StoragePath_NonEmpty_RoundTrips)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  defaultStoragePath: \"C:\\\\TestFolder\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(std::wstring(L"C:\\TestFolder"), s.Get<Setting::SessionStoragePath>());
        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
    }

    // An empty defaultStoragePath string is valid.
    TEST_METHOD(Validation_StoragePath_Empty_IsValid)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  defaultStoragePath: \"\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(std::wstring{}, s.Get<Setting::SessionStoragePath>());
        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
    }

    // Absent keys must silently use defaults — no warnings emitted.
    TEST_METHOD(Validation_AbsentKeys_NoWarningsAndDefaults)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(2048u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(102400u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(std::wstring{}, s.Get<Setting::SessionStoragePath>());
    }

    // Extra unknown keys at any level must not cause errors or warnings.
    TEST_METHOD(Validation_UnknownKeys_NoErrorsOrWarnings)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: 4\n"
            "  unknownSetting: 99\n"
            "unknownSection:\n"
            "  foo: bar\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Standard), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
    }
};

} // namespace WSLCCLISettingsUnitTests
