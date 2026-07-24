/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLISecretParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI --secret spec validation and parsing (validation::ParseSecretSpec).

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "ArgumentValidation.h"
#include "ImageService.h"
#include "Exceptions.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wsl::windows::wslc;

namespace WSLCCLISecretParserUnitTests {

// RAII helper: writes the given bytes to a uniquely named temp file and deletes it on destruction.
class ScopedTempFile
{
public:
    explicit ScopedTempFile(const std::vector<BYTE>& bytes)
    {
        const auto dir = std::filesystem::temp_directory_path();
        m_path =
            dir / (L"wslc_ut_secret_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++s_counter) + L".bin");
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        if (!bytes.empty())
        {
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    }

    ~ScopedTempFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    std::wstring wpath() const
    {
        return m_path.wstring();
    }

private:
    std::filesystem::path m_path;
    static inline int s_counter = 0;
};

class WSLCCLISecretParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLISecretParserUnitTests)

    static std::vector<BYTE> ToBytes(std::string_view text)
    {
        return std::vector<BYTE>(text.begin(), text.end());
    }

    // Parses a spec expected to be valid and asserts the resolved id and bytes.
    static void VerifyValid(const std::wstring& spec, const std::wstring& expectedId, const std::vector<BYTE>& expectedValue)
    {
        auto secret = validation::ParseSecretSpec(spec);
        VERIFY_ARE_EQUAL(expectedId, secret.Id);
        VERIFY_ARE_EQUAL(expectedValue.size(), secret.Value.size());
        VERIFY_IS_TRUE(expectedValue == secret.Value);
    }

    // Parses a spec expected to be rejected and asserts it throws an ArgumentException whose message is
    // the standard "Invalid --secret value '<spec>': <reason>" wrapper and contains the expected reason.
    static void VerifyInvalid(const std::wstring& spec, const std::wstring& expectedReasonSubstr)
    {
        try
        {
            (void)validation::ParseSecretSpec(spec);
            VERIFY_FAIL(L"Expected ArgumentException for invalid secret spec");
        }
        catch (const ArgumentException& ex)
        {
            const std::wstring& message = ex.Message();
            VERIFY_IS_TRUE(message.find(L"Invalid --secret value") != std::wstring::npos);
            VERIFY_IS_TRUE(message.find(expectedReasonSubstr) != std::wstring::npos);
        }
    }

    // --- Valid: environment-variable backed secrets ---

    TEST_METHOD(Secret_Env_BareIdReadsIdNamedVariable)
    {
        ScopedEnvVariable env(L"WSLC_UT_SECRET_BARE", L"bare-value");
        VerifyValid(L"id=WSLC_UT_SECRET_BARE", L"WSLC_UT_SECRET_BARE", ToBytes("bare-value"));
    }

    TEST_METHOD(Secret_Env_ExplicitEnvName)
    {
        ScopedEnvVariable env(L"WSLC_UT_SECRET_ENV", L"explicit-env");
        VerifyValid(L"id=my.secret,env=WSLC_UT_SECRET_ENV", L"my.secret", ToBytes("explicit-env"));
    }

    TEST_METHOD(Secret_Env_TypeEnvBareSrcIsVariableName)
    {
        ScopedEnvVariable env(L"WSLC_UT_SECRET_TYPEENV", L"type-env-src");
        VerifyValid(L"id=s,type=env,src=WSLC_UT_SECRET_TYPEENV", L"s", ToBytes("type-env-src"));
    }

    TEST_METHOD(Secret_Env_WinsOverSrcWhenBothPresent)
    {
        ScopedEnvVariable env(L"WSLC_UT_SECRET_ENVWINS", L"env-wins");
        // A non-existent src path is provided but must be ignored because env= takes precedence.
        VerifyValid(L"id=s,env=WSLC_UT_SECRET_ENVWINS,src=C:\\wslc-ut\\does-not-exist.txt", L"s", ToBytes("env-wins"));
    }

    TEST_METHOD(Secret_Env_ExplicitEnvUnsetYieldsEmptyValue)
    {
        // Ensure the variable is not set.
        ScopedEnvVariable env(L"WSLC_UT_SECRET_EXPLICIT_UNSET");
        VerifyValid(L"id=s,env=WSLC_UT_SECRET_EXPLICIT_UNSET", L"s", {});
    }

    TEST_METHOD(Secret_Env_EmptyVariableYieldsEmptyValue)
    {
        ScopedEnvVariable env(L"WSLC_UT_SECRET_EMPTY", L"");
        VerifyValid(L"id=WSLC_UT_SECRET_EMPTY", L"WSLC_UT_SECRET_EMPTY", {});
    }

    TEST_METHOD(Secret_Env_ValueEncodedAsUtf8)
    {
        // 'é' (U+00E9) encodes to the two UTF-8 bytes 0xC3 0xA9.
        ScopedEnvVariable env(L"WSLC_UT_SECRET_UTF8", L"h\u00e9llo");
        VerifyValid(L"id=WSLC_UT_SECRET_UTF8", L"WSLC_UT_SECRET_UTF8", {0x68, 0xC3, 0xA9, 0x6C, 0x6C, 0x6F});
    }

    TEST_METHOD(Secret_Env_IdAllowedCharacters)
    {
        ScopedEnvVariable env(L"WSLC_UT_SECRET_IDCHARS", L"ok");
        VerifyValid(L"id=Ab.9_-x,env=WSLC_UT_SECRET_IDCHARS", L"Ab.9_-x", ToBytes("ok"));
    }

    // --- Valid: file backed secrets ---

    TEST_METHOD(Secret_File_BareSrcReadsFileBytes)
    {
        ScopedTempFile file(ToBytes("file-content"));
        VerifyValid(L"id=s,src=" + file.wpath(), L"s", ToBytes("file-content"));
    }

    TEST_METHOD(Secret_File_TypeFileReadsFileBytes)
    {
        ScopedTempFile file(ToBytes("typed-file-content"));
        VerifyValid(L"id=s,type=file,src=" + file.wpath(), L"s", ToBytes("typed-file-content"));
    }

    TEST_METHOD(Secret_File_SourceKeyAlias)
    {
        ScopedTempFile file(ToBytes("aliased"));
        VerifyValid(L"id=s,source=" + file.wpath(), L"s", ToBytes("aliased"));
    }

    TEST_METHOD(Secret_File_EmptyFileYieldsEmptyValue)
    {
        ScopedTempFile file({});
        VerifyValid(L"id=s,src=" + file.wpath(), L"s", {});
    }

    TEST_METHOD(Secret_File_BinaryContentRoundTripsExactly)
    {
        const std::vector<BYTE> bytes = {0x00, 0x01, 0x02, 0xFF, 0x00, 0x41, 0x00, 0x7F, 0x80};
        ScopedTempFile file(bytes);
        VerifyValid(L"id=s,src=" + file.wpath(), L"s", bytes);
    }

    // --- Invalid: spec structure ---

    TEST_METHOD(Secret_Invalid_EmptyId)
    {
        VerifyInvalid(L"id=", L"'id=' is required");
    }

    TEST_METHOD(Secret_Invalid_MissingIdKey)
    {
        VerifyInvalid(L"env=WSLC_UT_SECRET_ANY", L"'id=' is required");
    }

    TEST_METHOD(Secret_Invalid_PartWithoutEquals)
    {
        VerifyInvalid(L"id=s,garbage", L"expected key=value pairs separated by ','");
    }

    TEST_METHOD(Secret_Invalid_PartWithLeadingEquals)
    {
        VerifyInvalid(L"=value", L"expected key=value pairs separated by ','");
    }

    TEST_METHOD(Secret_Invalid_UnsupportedKey)
    {
        VerifyInvalid(L"id=s,bogus=1", L"unsupported key 'bogus'");
    }

    // --- Invalid: id constraints ---

    TEST_METHOD(Secret_Invalid_IdStartsWithDash)
    {
        VerifyInvalid(L"id=-secret", L"'id' may not start with '-'");
    }

    TEST_METHOD(Secret_Invalid_IdContainsDisallowedCharacter)
    {
        VerifyInvalid(L"id=bad$id", L"'id' may only contain letters, digits");
    }

    TEST_METHOD(Secret_Invalid_IdContainsSlash)
    {
        VerifyInvalid(L"id=a/b", L"'id' may only contain letters, digits");
    }

    // --- Invalid: type constraints ---

    TEST_METHOD(Secret_Invalid_UnsupportedType)
    {
        VerifyInvalid(L"id=s,type=bogus", L"unsupported secret type 'bogus'");
    }

    TEST_METHOD(Secret_Invalid_TypeFileRequiresSrc)
    {
        VerifyInvalid(L"id=s,type=file", L"'type=file' requires 'src='");
    }

    // --- Invalid: value resolution ---

    TEST_METHOD(Secret_Invalid_SourceFileMissing)
    {
        VerifyInvalid(L"id=s,src=C:\\wslc-ut\\definitely-missing-secret-file.txt", L"source file not found or not a regular file");
    }

    TEST_METHOD(Secret_Invalid_BareIdVariableNotSet)
    {
        // A bare id whose matching environment variable is undefined must be rejected (Docker parity).
        ScopedEnvVariable env(L"WSLC_UT_SECRET_BARE_UNSET");
        VerifyInvalid(L"id=WSLC_UT_SECRET_BARE_UNSET", L"environment variable 'WSLC_UT_SECRET_BARE_UNSET' is not set");
    }
};

} // namespace WSLCCLISecretParserUnitTests
