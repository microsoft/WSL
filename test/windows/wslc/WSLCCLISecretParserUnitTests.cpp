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
        m_path = std::filesystem::temp_directory_path() /
                 (L"wslc_ut_secret_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++s_counter) + L".bin");
        std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
        THROW_HR_IF_MSG(E_FAIL, !file.is_open(), "Failed to create temp file: %ls", m_path.c_str());
        if (!bytes.empty())
        {
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            THROW_HR_IF_MSG(E_FAIL, !file.good(), "Failed to write temp file: %ls", m_path.c_str());
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

    // Parses a spec expected to resolve to a file-backed secret and asserts the resolved id, that no
    // bytes were read (Value is empty), and that the canonicalized source path is forwarded in place -
    // file secrets are delivered by mounting the file's directory into the VM, never by copying bytes.
    static void VerifyValidFileSecret(const std::wstring& spec, const std::wstring& expectedId, const std::wstring& expectedPath)
    {
        auto secret = validation::ParseSecretSpec(spec);
        VERIFY_ARE_EQUAL(expectedId, secret.Id);
        VERIFY_IS_TRUE(secret.Value.empty());
        std::error_code ec;
        const auto expectedCanonical = std::filesystem::weakly_canonical(std::filesystem::absolute(expectedPath, ec), ec);
        VERIFY_ARE_EQUAL(expectedCanonical.wstring(), secret.SourcePath);
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

    TEST_METHOD(Secret_File_BareSrcForwardsPath)
    {
        ScopedTempFile file(ToBytes("file-content"));
        VerifyValidFileSecret(L"id=s,src=" + file.wpath(), L"s", file.wpath());
    }

    TEST_METHOD(Secret_File_TypeFileForwardsPath)
    {
        ScopedTempFile file(ToBytes("typed-file-content"));
        VerifyValidFileSecret(L"id=s,type=file,src=" + file.wpath(), L"s", file.wpath());
    }

    TEST_METHOD(Secret_File_SourceKeyAlias)
    {
        ScopedTempFile file(ToBytes("aliased"));
        VerifyValidFileSecret(L"id=s,source=" + file.wpath(), L"s", file.wpath());
    }

    TEST_METHOD(Secret_File_QuotedFieldWithCommaInSrcPath)
    {
        // Docker parity: buildx parses --secret as a single CSV record, so a whole 'src=' field can be
        // double-quoted to carry a path containing commas; the comma must stay part of the value rather
        // than splitting into bogus extra key=value parts. This exercises SplitCsvFields end-to-end
        // through secret parsing. Note the entire "src=<path>" field is quoted (Go's CSV grammar), not
        // just the value - a bare quote after 'src=' would be an unquoted-field bare quote (malformed).
        const auto path =
            std::filesystem::temp_directory_path() / (L"wslc_ut_secret_" + std::to_wstring(GetCurrentProcessId()) + L"_a,b,c.bin");
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            VERIFY_IS_TRUE(file.is_open());
            file << 'x';
        }
        auto cleanup = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        });

        const std::wstring spec = L"id=s,\"src=" + path.wstring() + L"\"";
        auto secret = validation::ParseSecretSpec(spec);
        VERIFY_ARE_EQUAL(std::wstring(L"s"), secret.Id);
        VERIFY_IS_TRUE(secret.Value.empty());

        std::error_code ec;
        const auto expectedCanonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
        VERIFY_ARE_EQUAL(expectedCanonical.wstring(), secret.SourcePath);
    }

    TEST_METHOD(Secret_File_EmptyFileForwardsPath)
    {
        // An empty file is still a valid file secret: its path is forwarded and mounted (docker delivers
        // an empty /run/secrets/<id>); no bytes are carried in Value.
        ScopedTempFile file({});
        VerifyValidFileSecret(L"id=s,src=" + file.wpath(), L"s", file.wpath());
    }

    TEST_METHOD(Secret_File_BinaryFileForwardsPath)
    {
        // Binary content does not affect parsing: the file is referenced by path, not read, so arbitrary
        // bytes (including embedded NULs) are irrelevant to the client and delivered verbatim via mount.
        const std::vector<BYTE> bytes = {0x00, 0x01, 0x02, 0xFF, 0x00, 0x41, 0x00, 0x7F, 0x80};
        ScopedTempFile file(bytes);
        VerifyValidFileSecret(L"id=s,src=" + file.wpath(), L"s", file.wpath());
    }

    TEST_METHOD(Secret_File_LargeFileSucceeds)
    {
        // A large file must parse into a valid file secret.
        const std::vector<BYTE> bytes(512000, 0x41);
        ScopedTempFile file(bytes);
        VerifyValidFileSecret(L"id=s,src=" + file.wpath(), L"s", file.wpath());
    }

    TEST_METHOD(Secret_File_RelativeSrcResolvedToAbsolutePath)
    {
        // A relative src= must be resolved to an absolute SourcePath. The server rejects non-absolute
        // secret paths (the client and server may have different current directories), so the parser is
        // responsible for producing an absolute path before the spec is forwarded.
        ScopedTempFile file(ToBytes("relative-src"));
        const std::filesystem::path absPath = file.wpath();

        auto originalDir = std::filesystem::current_path();
        auto restoreDir = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::current_path(originalDir, ec);
        });
        std::filesystem::current_path(absPath.parent_path());

        const auto relativeSrc = absPath.filename().wstring();
        VERIFY_IS_FALSE(std::filesystem::path(relativeSrc).is_absolute());

        auto secret = validation::ParseSecretSpec(L"id=s,src=" + relativeSrc);
        VERIFY_ARE_EQUAL(std::wstring(L"s"), secret.Id);
        VERIFY_IS_TRUE(std::filesystem::path(secret.SourcePath).is_absolute());

        std::error_code ec;
        const auto expectedCanonical = std::filesystem::weakly_canonical(std::filesystem::absolute(absPath, ec), ec);
        VERIFY_ARE_EQUAL(expectedCanonical.wstring(), secret.SourcePath);
    }

    // A relative src= naming a file that does not exist must still resolve to an absolute SourcePath.
    // Parsing deliberately does not require the file to exist, so this case is reachable and the server
    // still rejects a non-absolute path. std::filesystem::weakly_canonical cannot handle it on its own:
    // it only produces an absolute path by canonicalizing the longest leading sequence of elements that
    // exist, so a bare missing filename has nothing to canonicalize and is returned unchanged. The
    // relative-src test above cannot catch this because its file exists.
    TEST_METHOD(Secret_File_RelativeSrcMissingFileResolvedToAbsolutePath)
    {
        const auto directory = std::filesystem::temp_directory_path();
        const auto relativeSrc = L"wslc_ut_secret_missing_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                 std::to_wstring(GetTickCount64()) + L".bin";
        VERIFY_IS_FALSE(std::filesystem::exists(directory / relativeSrc));

        auto originalDir = std::filesystem::current_path();
        auto restoreDir = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::current_path(originalDir, ec);
        });
        std::filesystem::current_path(directory);

        VERIFY_IS_FALSE(std::filesystem::path(relativeSrc).is_absolute());

        auto secret = validation::ParseSecretSpec(L"id=s,src=" + relativeSrc);
        VERIFY_ARE_EQUAL(std::wstring(L"s"), secret.Id);
        VERIFY_IS_TRUE(std::filesystem::path(secret.SourcePath).is_absolute());

        // The leading directory exists, so it canonicalizes; only the missing filename is appended.
        const auto expected = std::filesystem::canonical(directory) / relativeSrc;
        VERIFY_ARE_EQUAL(expected.wstring(), secret.SourcePath);
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

    TEST_METHOD(Secret_File_MissingSourceForwardsPath)
    {
        // A missing source file is not rejected client-side. The path is forwarded as
        // an absolute SourcePath and the service/BuildKit reports if it can't be mounted or read.
        const std::wstring missing = L"C:\\wslc-ut\\definitely-missing-secret-file.txt";
        auto secret = validation::ParseSecretSpec(L"id=s,src=" + missing);
        VERIFY_ARE_EQUAL(std::wstring(L"s"), secret.Id);
        VERIFY_IS_TRUE(secret.Value.empty());
        VERIFY_IS_TRUE(std::filesystem::path(secret.SourcePath).is_absolute());
    }

    TEST_METHOD(Secret_Invalid_BareIdVariableNotSet)
    {
        // A bare id whose matching environment variable is undefined must be rejected (Docker parity).
        ScopedEnvVariable env(L"WSLC_UT_SECRET_BARE_UNSET");
        VerifyInvalid(L"id=WSLC_UT_SECRET_BARE_UNSET", L"environment variable 'WSLC_UT_SECRET_BARE_UNSET' is not set");
    }
};

} // namespace WSLCCLISecretParserUnitTests
