/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIEnvVarParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI environment variable parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "ContainerModel.h"

#include <filesystem>
#include <fstream>

using namespace wsl::windows::wslc;

namespace WSLCCLIEnvVarParserUnitTests {

class WSLCCLIEnvVarParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIEnvVarParserUnitTests)

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnvTestFile = wsl::windows::common::filesystem::GetTempFilename();
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        DeleteFileW(EnvTestFile.c_str());
        return true;
    }

    TEST_METHOD(WSLCCLIEnvVarParser_ValidEnvVars)
    {
        const auto parsed = models::EnvironmentVariable::Parse(L"FOO=bar");
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(L"FOO=bar", parsed.value());
    }

    TEST_METHOD(WSLCCLIEnvVarParser_UsesProcessEnvWhenValueMissing)
    {
        constexpr const auto key = L"WSLC_TEST_ENV_FROM_PROCESS";
        VERIFY_IS_TRUE(SetEnvironmentVariableW(key, L"process_value"));

        auto cleanup = wil::scope_exit([&] { SetEnvironmentVariableW(key, nullptr); });

        const auto parsed = models::EnvironmentVariable::Parse(key);
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(L"WSLC_TEST_ENV_FROM_PROCESS=process_value", parsed.value());
    }

    TEST_METHOD(WSLCCLIEnvVarParser_NulloptForWhitespaceOrUnsetVar)
    {
        const auto whitespaceOnly = models::EnvironmentVariable::Parse(L"   \t  ");
        VERIFY_IS_FALSE(whitespaceOnly.has_value());

        SetEnvironmentVariableA("WSLC_TEST_ENV_UNSET", nullptr);
        const auto missingFromProcess = models::EnvironmentVariable::Parse(L"WSLC_TEST_ENV_UNSET");
        VERIFY_IS_FALSE(missingFromProcess.has_value());
    }

    TEST_METHOD(WSLCCLIEnvVarParser_InvalidKeysThrow)
    {
        auto verifyThrowsWithMessage = [](const std::wstring& input, const std::wstring& expectedSubstring) {
            try
            {
                (void)models::EnvironmentVariable::Parse(input);
                VERIFY_FAIL(L"Expected exception");
            }
            catch (const wil::ResultException& ex)
            {
                VERIFY_ARE_EQUAL(E_INVALIDARG, ex.GetErrorCode());

                const auto raw = ex.GetFailureInfo().pszMessage;
                std::wstring message = raw ? raw : L"";
                VERIFY_ARE_EQUAL(expectedSubstring, message);
            }
        };

        verifyThrowsWithMessage(L"=value", L"Environment variable key cannot be empty");
        verifyThrowsWithMessage(L"BAD KEY=value", L"Environment variable key 'BAD KEY' cannot contain whitespace");
        verifyThrowsWithMessage(L"BAD\tKEY=value", L"Environment variable key 'BAD\tKEY' cannot contain whitespace");
        verifyThrowsWithMessage(L"BAD\nKEY=value", L"Environment variable key 'BAD\nKEY' cannot contain whitespace");
    }

    TEST_METHOD(WSLCCLIEnvVarParser_ParseFileParsesAndSkipsExpectedLines)
    {
        constexpr const auto key = L"WSLC_TEST_ENV_FROM_FILE";
        VERIFY_IS_TRUE(SetEnvironmentVariableW(key, L"file_process_value") == TRUE);

        auto envCleanup = wil::scope_exit([&] { SetEnvironmentVariableW(key, nullptr); });

        std::ofstream file(EnvTestFile);
        VERIFY_IS_TRUE(file.is_open());
        file << "# comment\n";
        file << "\n";
        file << "KEY1=VALUE1\n";
        file << "   KEY2=VALUE2\n";
        file << "WSLC_TEST_ENV_FROM_FILE\n";
        file << "WSLC_TEST_ENV_DOES_NOT_EXIST\n";
        file.close();

        const auto parsed = models::EnvironmentVariable::ParseFile(EnvTestFile.wstring());

        VERIFY_ARE_EQUAL(3U, parsed.size());
        VERIFY_ARE_EQUAL(L"KEY1=VALUE1", parsed[0]);
        VERIFY_ARE_EQUAL(L"KEY2=VALUE2", parsed[1]);
        VERIFY_ARE_EQUAL(L"WSLC_TEST_ENV_FROM_FILE=file_process_value", parsed[2]);
    }

    TEST_METHOD(WSLCCLIEnvVarParser_ParseFileThrowsWhenMissing)
    {
        try
        {
            (void)models::EnvironmentVariable::ParseFile(L"ENV_FILE_NOT_FOUND");
            VERIFY_FAIL(L"Expected exception");
        }
        catch (const wil::ResultException& ex)
        {
            VERIFY_ARE_EQUAL(E_INVALIDARG, ex.GetErrorCode());

            const auto raw = ex.GetFailureInfo().pszMessage;
            std::wstring message = raw ? raw : L"";
            VERIFY_ARE_EQUAL(L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading", message);
        }
    }

    TEST_METHOD(WSLCCLIEnvVarParser_ExplicitEmptyValueIsValid)
    {
        const auto parsed = models::EnvironmentVariable::Parse(L"FOO=");
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(L"FOO=", parsed.value());
    }

    TEST_METHOD(WSLCCLIEnvVarParser_MultipleEqualsPreservedInValue)
    {
        const auto parsed = models::EnvironmentVariable::Parse(L"FOO=a=b=c");
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(L"FOO=a=b=c", parsed.value());
    }

    TEST_METHOD(WSLCCLIEnvVarParser_EmptyInputReturnsNullopt)
    {
        const auto parsed = models::EnvironmentVariable::Parse(L"");
        VERIFY_IS_FALSE(parsed.has_value());
    }

    TEST_METHOD(WSLCCLIEnvVarParser_UsesProcessEnvWhenValueIsExplicitlyEmpty)
    {
        constexpr const auto key = L"WSLC_TEST_ENV_EMPTY_VALUE";
        VERIFY_IS_TRUE(SetEnvironmentVariableW(key, L""));

        auto cleanup = wil::scope_exit([&] { SetEnvironmentVariableW(key, nullptr); });

        const auto parsed = models::EnvironmentVariable::Parse(key);
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(L"WSLC_TEST_ENV_EMPTY_VALUE=", parsed.value());
    }

    TEST_METHOD(WSLCCLIEnvVarParser_ParseFilePreservesTrailingWhitespaceInValue)
    {
        std::ofstream file(EnvTestFile);
        VERIFY_IS_TRUE(file.is_open());
        file << "KEY=value   \n";
        file.close();

        const auto parsed = models::EnvironmentVariable::ParseFile(EnvTestFile.wstring());

        VERIFY_ARE_EQUAL(1U, parsed.size());
        VERIFY_ARE_EQUAL(L"KEY=value   ", parsed[0]);
    }

    TEST_METHOD(WSLCCLIEnvVarParser_ParseFileThrowsOnInvalidLine)
    {
        try
        {
            std::ofstream file(EnvTestFile);
            VERIFY_IS_TRUE(file.is_open());
            file << "BAD KEY=value\n";
            file.close();

            (void)models::EnvironmentVariable::ParseFile(EnvTestFile.wstring());
            VERIFY_FAIL(L"Expected exception");
        }
        catch (const wil::ResultException& ex)
        {
            VERIFY_ARE_EQUAL(E_INVALIDARG, ex.GetErrorCode());

            const auto raw = ex.GetFailureInfo().pszMessage;
            std::wstring message = raw ? raw : L"";
            VERIFY_ARE_EQUAL(L"Environment variable key 'BAD KEY' cannot contain whitespace", message);
        }
    }

    TEST_METHOD(WSLCCLIEnvVarParser_ParseFileEmptyFileReturnsEmpty)
    {
        std::ofstream file(EnvTestFile);
        VERIFY_IS_TRUE(file.is_open());
        file.close();

        const auto parsed = models::EnvironmentVariable::ParseFile(EnvTestFile.wstring());
        VERIFY_ARE_EQUAL(0U, parsed.size());
    }

private:
    std::filesystem::path EnvTestFile;
};

} // namespace WSLCCLIEnvVarParserUnitTests