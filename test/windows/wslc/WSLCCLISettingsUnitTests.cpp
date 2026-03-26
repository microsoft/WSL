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
        VERIFY_ARE_EQUAL(map.GetOrDefault<Setting::SessionCpuCount>(), 4u);
        VERIFY_ARE_EQUAL(map.GetOrDefault<Setting::SessionMemoryMb>(), 2048u);
        VERIFY_ARE_EQUAL(map.GetOrDefault<Setting::SessionStorageSizeMb>(), 10000u);
        VERIFY_ARE_EQUAL(map.GetOrDefault<Setting::SessionStoragePath>(), std::wstring{});
    }

    // After inserting a value, GetOrDefault must return it rather than the default.
    TEST_METHOD(SettingsMap_GetOrDefault_ReturnsStoredWhenPresent)
    {
        SettingsMap map;
        map.Add<Setting::SessionCpuCount>(16u);
        VERIFY_ARE_EQUAL(map.GetOrDefault<Setting::SessionCpuCount>(), 16u);
        VERIFY_ARE_EQUAL(map.GetOrDefault<Setting::SessionMemoryMb>(), 2048u);
    }

    // -----------------------------------------------------------------------
    // Default (neither file exists)
    // -----------------------------------------------------------------------

    // When neither primary nor backup exists the type must be Default, there
    // must be no warnings, and all values must be at their built-in defaults.
    TEST_METHOD(LoadSettings_NoFiles_YieldsDefaultTypeAndNoWarnings)
    {
        UserSettingsTest s{UniqueTempDir()};

        VERIFY_ARE_EQUAL(static_cast<int>(s.GetType()), static_cast<int>(UserSettingsType::Default));
        VERIFY_ARE_EQUAL(s.GetWarnings().size(), 0u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 4u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionMemoryMb>(), 2048u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStorageSizeMb>(), 10000u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStoragePath>(), std::wstring{});
    }

    // -----------------------------------------------------------------------
    // Standard (valid primary)
    // -----------------------------------------------------------------------

    // A well-formed primary file must set the type to Standard with no
    // warnings and the specified values loaded.
    TEST_METHOD(LoadSettings_ValidPrimary_YieldsStandardTypeAndValues)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"UserSettings.yaml",
            "session:\n"
            "  cpuCount: 8\n"
            "  memorySizeMb: 4096\n"
            "  maxStorageSizeMb: 20000\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(s.GetType()), static_cast<int>(UserSettingsType::Standard));
        VERIFY_ARE_EQUAL(s.GetWarnings().size(), 0u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 8u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionMemoryMb>(), 4096u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStorageSizeMb>(), 20000u);
        // Unspecified setting falls back to built-in default.
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStoragePath>(), std::wstring{});
    }

    // An empty primary file is valid YAML (null document); all settings use
    // their defaults with no warnings.
    TEST_METHOD(LoadSettings_EmptyPrimary_AllDefaultsNoWarnings)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.GetWarnings().size(), 0u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 4u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionMemoryMb>(), 2048u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStorageSizeMb>(), 10000u);
    }

    // -----------------------------------------------------------------------
    // Backup fallback
    // -----------------------------------------------------------------------

    // When the primary exists but fails to parse, the backup file is used and
    // the type is Backup.
    TEST_METHOD(LoadSettings_InvalidPrimary_ValidBackup_YieldsBackupType)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session: [\n"); // broken YAML
        WriteFile(dir / L"UserSettings.yaml.bak", "session:\n  cpuCount: 2\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(s.GetType()), static_cast<int>(UserSettingsType::Backup));
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 2u);
    }

    // When the primary is absent and only the backup exists, the type is Backup.
    TEST_METHOD(LoadSettings_NoPrimary_ValidBackup_YieldsBackupType)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml.bak", "session:\n  memorySizeMb: 1024\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(s.GetType()), static_cast<int>(UserSettingsType::Backup));
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionMemoryMb>(), 1024u);
    }

    // When both files fail to parse, the type is Default and at least one
    // warning is emitted per parse failure.
    TEST_METHOD(LoadSettings_BothInvalid_YieldsDefaultTypeWithWarnings)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", ": bad: [\n");       // broken YAML (unclosed flow seq)
        WriteFile(dir / L"UserSettings.yaml.bak", "session: [\n"); // broken YAML (unclosed flow seq)

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(s.GetType()), static_cast<int>(UserSettingsType::Default));
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 2u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 4u);
    }

    // -----------------------------------------------------------------------
    // Per-setting validation
    // -----------------------------------------------------------------------

    // cpuCount: 0 must be rejected; the default (4) is used and a warning emitted.
    TEST_METHOD(Validation_CpuCount_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session:\n  cpuCount: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 4u);
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
        VERIFY_IS_FALSE(s.GetWarnings().front().SettingPath.empty());
    }

    // memorySizeMb: 0 must be rejected; the default (2048) is used.
    TEST_METHOD(Validation_MemoryMb_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session:\n  memorySizeMb: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.Get<Setting::SessionMemoryMb>(), 2048u);
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
    }

    // maxStorageSizeMb: 0 must be rejected; the default (10000) is used.
    TEST_METHOD(Validation_StorageSizeMb_Zero_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session:\n  maxStorageSizeMb: 0\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStorageSizeMb>(), 10000u);
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
    }

    // A string where a uint32_t is expected must emit a type warning and fall
    // back to the default.
    TEST_METHOD(Validation_WrongType_UsesDefaultAndWarns)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session:\n  cpuCount: \"not-a-number\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 4u);
        VERIFY_IS_TRUE(s.GetWarnings().size() >= 1u);
    }

    // A valid defaultStoragePath string must survive the UTF-8 → wstring round-trip.
    TEST_METHOD(Validation_StoragePath_NonEmpty_RoundTrips)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session:\n  defaultStoragePath: \"/mnt/data/storage\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStoragePath>(), std::wstring(L"/mnt/data/storage"));
        VERIFY_ARE_EQUAL(s.GetWarnings().size(), 0u);
    }

    // An empty defaultStoragePath string is valid.
    TEST_METHOD(Validation_StoragePath_Empty_IsValid)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session:\n  defaultStoragePath: \"\"\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStoragePath>(), std::wstring{});
        VERIFY_ARE_EQUAL(s.GetWarnings().size(), 0u);
    }

    // Absent keys must silently use defaults — no warnings emitted.
    TEST_METHOD(Validation_AbsentKeys_NoWarningsAndDefaults)
    {
        auto dir = UniqueTempDir();
        WriteFile(dir / L"UserSettings.yaml", "session:\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(s.GetWarnings().size(), 0u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 4u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionMemoryMb>(), 2048u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStorageSizeMb>(), 10000u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionStoragePath>(), std::wstring{});
    }

    // Extra unknown keys at any level must not cause errors or warnings.
    TEST_METHOD(Validation_UnknownKeys_NoErrorsOrWarnings)
    {
        auto dir = UniqueTempDir();
        WriteFile(
            dir / L"UserSettings.yaml",
            "session:\n"
            "  cpuCount: 4\n"
            "  unknownSetting: 99\n"
            "unknownSection:\n"
            "  foo: bar\n");

        UserSettingsTest s{dir};

        VERIFY_ARE_EQUAL(static_cast<int>(s.GetType()), static_cast<int>(UserSettingsType::Standard));
        VERIFY_ARE_EQUAL(s.GetWarnings().size(), 0u);
        VERIFY_ARE_EQUAL(s.Get<Setting::SessionCpuCount>(), 4u);
    }
};

} // namespace WSLCCLISettingsUnitTests
