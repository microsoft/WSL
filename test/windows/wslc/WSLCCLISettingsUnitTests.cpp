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
#include "WSLCUserSettings.h"

#include <atomic>
#include <fstream>

using namespace wsl::windows::wslc::settings;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;
using Loc = wsl::shared::Localization;

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
        VERIFY_ARE_EQUAL(0u, map.GetOrDefault<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, map.GetOrDefault<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(1048576u, map.GetOrDefault<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(static_cast<int>(CredentialStoreType::WinCred), static_cast<int>(map.GetOrDefault<Setting::CredentialStore>()));
    }

    // After inserting a value, GetOrDefault must return it rather than the default.
    TEST_METHOD(SettingsMap_GetOrDefault_ReturnsStoredWhenPresent)
    {
        SettingsMap map;
        map.Add<Setting::SessionCpuCount>(16u);
        VERIFY_ARE_EQUAL(16u, map.GetOrDefault<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, map.GetOrDefault<Setting::SessionMemoryMb>());
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
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(1048576u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(static_cast<int>(CredentialStoreType::WinCred), static_cast<int>(s.Get<Setting::CredentialStore>()));
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
            "  maxStorageSize: 20000MB\n"
            "credentialStore: file\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Standard), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(8u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(4096u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(20000u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(static_cast<int>(CredentialStoreType::File), static_cast<int>(s.Get<Setting::CredentialStore>()));
    }

    // An empty settings file is valid YAML (null document) but not a mapping;
    // a structure warning is emitted and all settings use defaults.
    TEST_METHOD(LoadSettings_EmptySettings_WarnsInvalidStructure)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Standard), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(Loc::WSLCUserSettings_Warning_InvalidStructure(s.SettingsFilePath().wstring()), s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(1048576u, s.Get<Setting::SessionStorageSizeMb>());
    }

    // A non-map root (e.g. bare scalar) is valid YAML but invalid structure;
    // a warning is emitted and all settings use defaults.
    TEST_METHOD(LoadSettings_NonMapRoot_WarnsInvalidStructure)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "just a string\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Standard), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(Loc::WSLCUserSettings_Warning_InvalidStructure(s.SettingsFilePath().wstring()), s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
    }

    // When the settings file fails to parse, the type is Default and a warning is emitted.
    TEST_METHOD(LoadSettings_InvalidSettings_YieldsDefaultTypeWithWarning)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session: [\n"); // broken YAML (unclosed flow seq)

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Default), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        // Parse errors include yaml-cpp details, so check prefix including the file path.
        VERIFY_IS_TRUE(s.GetWarnings().front().Message.starts_with(
            L"Warning: Settings file at " + s.SettingsFilePath().wstring() + L" could not be parsed."));
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
    }

    // -----------------------------------------------------------------------
    // Per-setting validation
    // -----------------------------------------------------------------------

    // cpuCount: 0 is rejected by validation; the default (0) is used and a warning emitted.
    TEST_METHOD(Validation_CpuCount_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  cpuCount: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_InvalidValue(L"session.cpuCount", s.SettingsFilePath().wstring(), 2),
            s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(std::wstring(L"session.cpuCount"), s.GetWarnings().front().SettingPath);
    }

    // memorySize: 0 is rejected by validation; the default (0) is used.
    TEST_METHOD(Validation_MemoryMb_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  memorySize: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_InvalidValue(L"session.memorySize", s.SettingsFilePath().wstring(), 2),
            s.GetWarnings().front().Message);
    }

    // maxStorageSize: 0 must be rejected; the default is used.
    TEST_METHOD(Validation_StorageSizeMb_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  maxStorageSize: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(1048576u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_InvalidValue(L"session.maxStorageSize", s.SettingsFilePath().wstring(), 2),
            s.GetWarnings().front().Message);
    }

    // A string where a uint32_t is expected must emit a type warning and fall
    // back to the default.
    TEST_METHOD(Validation_WrongType_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n  cpuCount: \"not-a-number\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_InvalidType(L"session.cpuCount", s.SettingsFilePath().wstring(), 2),
            s.GetWarnings().front().Message);
    }

    // Absent keys must silently use defaults — no warnings emitted.
    TEST_METHOD(Validation_AbsentKeys_NoWarningsAndDefaults)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "session:\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(1048576u, s.Get<Setting::SessionStorageSizeMb>());
    }

    // The string "default" for any setting must silently use the built-in
    // default, same as if the key were absent.
    TEST_METHOD(Validation_DefaultString_UsesBuiltInDefaultsNoWarnings)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: default\n"
            "  memorySize: default\n"
            "  maxStorageSize: default\n"
            "  networkingMode: default\n"
            "  hostFileShareMode: default\n"
            "  dnsTunneling: default\n"
            "credentialStore: default\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(UserSettingsType::Standard), static_cast<int>(s.GetType()));
        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(1048576u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(static_cast<int>(WSLCNetworkingModeVirtioProxy), static_cast<int>(s.Get<Setting::SessionNetworkingMode>()));
        VERIFY_ARE_EQUAL(static_cast<int>(HostFileShareMode::VirtioFs), static_cast<int>(s.Get<Setting::SessionHostFileShareMode>()));
        VERIFY_IS_TRUE(s.Get<Setting::SessionDnsTunneling>());
        VERIFY_ARE_EQUAL(static_cast<int>(CredentialStoreType::WinCred), static_cast<int>(s.Get<Setting::CredentialStore>()));
    }

    // "default" on a single setting uses the built-in default for that setting
    // while explicit values on other settings are preserved.
    TEST_METHOD(Validation_DefaultString_MixedWithExplicitValues)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: 8\n"
            "  memorySize: default\n"
            "  maxStorageSize: 50000MB\n"
            "credentialStore: default\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(8u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionMemoryMb>());
        VERIFY_ARE_EQUAL(50000u, s.Get<Setting::SessionStorageSizeMb>());
        VERIFY_ARE_EQUAL(static_cast<int>(CredentialStoreType::WinCred), static_cast<int>(s.Get<Setting::CredentialStore>()));
    }

    // Quoted "default" string must behave the same as unquoted default.
    TEST_METHOD(Validation_DefaultString_QuotedIsAlsoValid)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: \"default\"\n"
            "  memorySize: \"default\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(0u, s.Get<Setting::SessionMemoryMb>());
    }

    // "Default" (capitalized) is NOT the magic string — it must be treated as
    // an invalid value and fall back to the built-in default with a warning.
    TEST_METHOD(Validation_DefaultString_IsCaseSensitive)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  networkingMode: Default\n"
            "credentialStore: DEFAULT\n");

        UserSettingsTest s{dir};

        // Both should be rejected by their validators and produce warnings.
        VERIFY_ARE_EQUAL(2u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_InvalidValue(L"session.networkingMode", s.SettingsFilePath().wstring(), 2),
            s.GetWarnings()[0].Message);
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_InvalidValue(L"credentialStore", s.SettingsFilePath().wstring(), 3), s.GetWarnings()[1].Message);
        // Values still fall back to built-in defaults.
        VERIFY_ARE_EQUAL(static_cast<int>(WSLCNetworkingModeVirtioProxy), static_cast<int>(s.Get<Setting::SessionNetworkingMode>()));
        VERIFY_ARE_EQUAL(static_cast<int>(CredentialStoreType::WinCred), static_cast<int>(s.Get<Setting::CredentialStore>()));
    }

    // credentialStore: invalid value must fall back to default and warn.
    TEST_METHOD(Validation_CredentialStore_Invalid_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"settings.yaml", "credentialStore: badvalue\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(CredentialStoreType::WinCred), static_cast<int>(s.Get<Setting::CredentialStore>()));
        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_InvalidValue(L"credentialStore", s.SettingsFilePath().wstring(), 1),
            s.GetWarnings().front().Message);
    }

    // -----------------------------------------------------------------------
    // Unknown key warnings
    // -----------------------------------------------------------------------

    // Unknown keys in a known section and unknown root sections both produce warnings.
    TEST_METHOD(Validation_UnknownKeys_WarnsAboutUnknownKeys)
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
        VERIFY_ARE_EQUAL(4u, s.Get<Setting::SessionCpuCount>());
        VERIFY_ARE_EQUAL(2u, s.GetWarnings().size());
        // Root-level keys are processed before nested keys due to stack-based traversal.
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_UnknownSection(L"unknownSection", s.SettingsFilePath().wstring(), 4), s.GetWarnings()[0].Message);
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_UnknownKey(L"session.unknownSetting", s.SettingsFilePath().wstring(), 3),
            s.GetWarnings()[1].Message);
    }

    // An unknown key under a known section produces a warning with the full path.
    TEST_METHOD(Validation_UnknownKeys_UnknownInKnownSection)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: 4\n"
            "  typoSetting: true\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_UnknownKey(L"session.typoSetting", s.SettingsFilePath().wstring(), 3),
            s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(std::wstring(L"session.typoSetting"), s.GetWarnings().front().SettingPath);
    }

    // An unknown root-level section produces a warning.
    TEST_METHOD(Validation_UnknownKeys_UnknownRootSection)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "badSection:\n"
            "  key: value\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_UnknownSection(L"badSection", s.SettingsFilePath().wstring(), 1),
            s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(std::wstring(L"badSection"), s.GetWarnings().front().SettingPath);
    }

    // An unknown root-level scalar key produces a warning.
    TEST_METHOD(Validation_UnknownKeys_UnknownRootScalar)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: 4\n"
            "badKey: hello\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_UnknownKey(L"badKey", s.SettingsFilePath().wstring(), 3), s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(std::wstring(L"badKey"), s.GetWarnings().front().SettingPath);
    }

    // A complex YAML key (sequence) cannot be converted to string;
    // a non-string key warning is emitted.
    TEST_METHOD(Validation_UnknownKeys_ComplexKey_WarnsNonStringKey)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: 4\n"
            "  [1, 2]: value\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_NonStringKey(L"session", s.SettingsFilePath().wstring(), 3), s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(std::wstring(L"session"), s.GetWarnings().front().SettingPath);
    }

    // A complex YAML key (map) at root level warns with "root" location.
    TEST_METHOD(Validation_UnknownKeys_ComplexKeyAtRoot_WarnsNonStringKey)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "{a: b}: value\n"
            "credentialStore: wincred\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(1u, s.GetWarnings().size());
        VERIFY_ARE_EQUAL(
            Loc::WSLCUserSettings_Warning_NonStringKey(L"root", s.SettingsFilePath().wstring(), 1), s.GetWarnings().front().Message);
        VERIFY_ARE_EQUAL(std::wstring(L"root"), s.GetWarnings().front().SettingPath);
    }

    // A file with only valid known keys produces no warnings.
    TEST_METHOD(Validation_UnknownKeys_AllKnownKeys_NoWarnings)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"settings.yaml",
            "session:\n"
            "  cpuCount: 8\n"
            "  memorySize: 4GB\n"
            "  maxStorageSize: 50000MB\n"
            "  networkingMode: nat\n"
            "  hostFileShareMode: virtiofs\n"
            "  dnsTunneling: true\n"
            "credentialStore: wincred\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(0u, s.GetWarnings().size());
    }
};

} // namespace WSLCCLISettingsUnitTests
