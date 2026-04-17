/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVolumeMountUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC volume mount argument parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "ContainerModel.h"

using namespace wsl::windows::wslc::models;

namespace WSLCVolumeMount {

class WSLCVolumeMountUnitTests
{
    WSL_TEST_CLASS(WSLCVolumeMountUnitTests)

    TEST_METHOD(VolumeMount_Parse_ReturnExpectedResult)
    {
        const auto cwd = std::filesystem::current_path();

        // Volume value => host, container, readonly
        std::vector<std::tuple<std::wstring, std::wstring, std::string, bool>> validVolumeArgs = {
            {LR"(C:\hostPath:/containerPath)", LR"(C:\hostPath)", R"(/containerPath)", false},
            {LR"(C:\hostPath:/containerPath:ro)", LR"(C:\hostPath)", R"(/containerPath)", true},
            {LR"(C:\hostPath:/containerPath:rw)", LR"(C:\hostPath)", R"(/containerPath)", false},
            {LR"(C:\host Path:/container Path:ro)", LR"(C:\host Path)", R"(/container Path)", true},
            {LR"(C:\host Path:/container Path:rw)", LR"(C:\host Path)", R"(/container Path)", false},

            // Relative paths. Expected host is CWD + the normalized relative component.
            // Windows will convert forward slashes to backslashes in the host path.
            {L"./foo:/data", (cwd / L"foo").wstring(), R"(/data)", false},
            {L".\\foo:/data", (cwd / L"foo").wstring(), R"(/data)", false},
            {L"./foo:/data:ro", (cwd / L"foo").wstring(), R"(/data)", true},
            {L"../bar:/data:rw", (cwd.parent_path() / L"bar").wstring(), R"(/data)", false},
            {L"..\\bar:/data:rw", (cwd.parent_path() / L"bar").wstring(), R"(/data)", false},
            {L"sub/dir:/data", (cwd / L"sub" / L"dir").wstring(), R"(/data)", false},
            {L"sub\\dir:/data", (cwd / L"sub" / L"dir").wstring(), R"(/data)", false},
        };

        for (const auto& arg : validVolumeArgs)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing volume argument: '{}'", std::get<0>(arg)).c_str());
            auto result = VolumeMount::Parse(std::get<0>(arg));
            VERIFY_ARE_EQUAL(std::get<1>(arg), result.HostPath());
            VERIFY_ARE_EQUAL(std::get<2>(arg), result.ContainerPath());
            VERIFY_ARE_EQUAL(std::get<3>(arg), result.IsReadOnly());
        }
    }

    TEST_METHOD(VolumeMount_Parse_InvalidArgs)
    {
        std::vector<std::wstring> invalidCases = {
            LR"(:/containerPath)",             // Empty host path
            LR"(:/containerPath:ro)",          // Empty host path
            LR"(:)",                           // Empty container path
            LR"(::)",                          // Empty container path
            LR"(C:\hostPath::ro)",             // Empty container path
            LR"(C:\hostPath:)",                // Empty container path
            LR"(C:\hostPath::rw)",             // Empty container path
            LR"(C:\hostPath:/containerPath:)", // Empty container path
        };

        for (const auto& value : invalidCases)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing invalid volume argument: '{}'", value).c_str());
            VERIFY_THROWS(VolumeMount::Parse(value), wil::ResultException);
        }
    }

    TEST_METHOD(VolumeMount_Parse_InvalidContainerPath)
    {
        // Drive colon gets misinterpreted as the host:container separator,
        // producing a non-absolute container path. Caught by container path validation.
        std::vector<std::wstring> invalidPathCases = {
            LR"(C:\hostPath:ro)",                          // host='C', container=\hostPath (not absolute)
            LR"(C:\hostPath)",                             // host='C', container=\hostPath (not absolute)
            LR"(C:\hostPath:/containerPath:invalid_mode)", // container=invalid_mode (not absolute)
            LR"(C:\hostPath:/containerPath:ro:extra)",     // container=extra (not absolute)
        };

        for (const auto& value : invalidPathCases)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing invalid path: '{}'", value).c_str());
            VERIFY_THROWS(VolumeMount::Parse(value), wil::ResultException);
        }
    }

    TEST_METHOD(VolumeMount_IsValidNamedVolumeName_ValidNames)
    {
        // These should all be recognised as valid Docker named volume names.
        std::vector<std::wstring> validNames = {
            L"myvolume",   // simple lowercase
            L"MyVolume",   // mixed case
            L"my-volume",  // hyphen
            L"my_volume",  // underscore
            L"my.volume",  // dot
            L"v1",         // minimum length (2 chars)
            L"volume123",  // trailing digits
            L"my-vol_1.0", // combination of all allowed characters
        };

        for (const auto& name : validNames)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing valid named volume name: '{}'", name).c_str());
            VERIFY_IS_TRUE(VolumeMount::IsValidNamedVolumeName(name));
        }
    }

    TEST_METHOD(VolumeMount_IsValidNamedVolumeName_InvalidNames)
    {
        // These should all be rejected. They are either paths or otherwise invalid.
        std::vector<std::wstring> invalidNames = {
            L"./foo",             // relative path with ./
            L"../foo",            // relative path with ../
            L".hidden",           // starts with '.' (relative path indicator)
            L"foo/bar",           // contains forward slash (path separator)
            L"foo\\bar",          // contains backslash (path separator)
            L"C:\\path\\to\\dir", // absolute Windows path
            L"/absolute/path",    // absolute Unix-style path
            L"a",                 // too short (single character)
            L"",                  // empty
            L":volume",           // starts with invalid character
            L"-volume",           // starts with hyphen (not alphanumeric)
            L"_volume",           // starts with underscore (not alphanumeric)
            L"vol ume",           // contains space
            L"vol:ume",           // contains colon
        };

        for (const auto& name : invalidNames)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing invalid named volume name: '{}'", name).c_str());
            VERIFY_IS_FALSE(VolumeMount::IsValidNamedVolumeName(name));
        }
    }
};
} // namespace WSLCVolumeMount