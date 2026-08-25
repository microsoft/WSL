/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    FilesystemUnitTests.cpp

Abstract:

    This file contains unit tests for the helpers in src/windows/common/filesystem.cpp.
    These tests only read from the local filesystem so they do not require an installed distribution.

--*/

#include "precomp.h"
#include "Common.h"

using wsl::windows::common::filesystem::GetCanonicalPath;

namespace {

// Returns a file name that does not exist in the given directory.
std::wstring UniqueMissingName(const std::filesystem::path& Directory)
{
    static int counter = 0;
    const auto name = std::format(L"wsl_ut_canonical_{}_{}.txt", GetCurrentProcessId(), ++counter);
    VERIFY_IS_FALSE(std::filesystem::exists(Directory / name));

    return name;
}

// The canonical form of the current directory, which is what a relative path is expected to resolve
// against. std::filesystem::canonical is used rather than weakly_canonical so the expected value is
// computed independently of the API under test.
std::filesystem::path CanonicalCurrentDirectory()
{
    return std::filesystem::canonical(std::filesystem::current_path());
}

// A path that std::filesystem::absolute is guaranteed to reject, because it exceeds the longest path
// Win32 can express. An empty path is not used: whether absolute rejects one is implementation
// defined, and some standard library versions accept it.
std::filesystem::path UnresolvablePath()
{
    return {L"C:\\" + std::wstring(40000, L'a')};
}

// A file that is known to exist, used to cover paths that resolve to a real filesystem entry. The
// test module itself is used so that no file has to be created.
std::filesystem::path ExistingFile()
{
    return {wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle())};
}

} // namespace

namespace FilesystemUnitTests {
class FilesystemUnitTests
{
    WSL_TEST_CLASS(FilesystemUnitTests)

    // A relative path naming a file that does not exist must still resolve to an absolute path.
    // std::filesystem::weakly_canonical cannot do this on its own: it builds its result from the
    // longest leading sequence of elements that exist, so a bare missing file name has nothing to
    // canonicalize and is returned unchanged.
    TEST_METHOD(GetCanonicalPath_RelativeMissingPathIsMadeAbsolute)
    {
        const auto name = UniqueMissingName(std::filesystem::current_path());
        VERIFY_IS_FALSE(std::filesystem::weakly_canonical(name).is_absolute());

        const auto result = GetCanonicalPath(name);

        VERIFY_IS_TRUE(result.is_absolute());
        VERIFY_ARE_EQUAL((CanonicalCurrentDirectory() / name).wstring(), result.wstring());
    }

    // The same resolution must happen for a relative path whose target already exists.
    TEST_METHOD(GetCanonicalPath_RelativeExistingPathIsMadeAbsolute)
    {
        const auto existing = ExistingFile();
        const auto relativePath = std::filesystem::relative(existing, std::filesystem::current_path());
        VERIFY_IS_FALSE(relativePath.empty());
        VERIFY_IS_FALSE(relativePath.is_absolute());

        const auto result = GetCanonicalPath(relativePath);

        VERIFY_IS_TRUE(result.is_absolute());
        VERIFY_ARE_EQUAL(std::filesystem::canonical(existing).wstring(), result.wstring());
    }

    // '.' and '..' components must be collapsed even when the intermediate directory does not exist.
    TEST_METHOD(GetCanonicalPath_CollapsesDotSegments)
    {
        const auto name = UniqueMissingName(std::filesystem::current_path());

        const auto result = GetCanonicalPath(L".\\nonexistent\\..\\" + name);

        VERIFY_ARE_EQUAL((CanonicalCurrentDirectory() / name).wstring(), result.wstring());
    }

    // An already absolute path must be returned unchanged.
    TEST_METHOD(GetCanonicalPath_AbsolutePathIsUnchanged)
    {
        const auto expected = CanonicalCurrentDirectory() / UniqueMissingName(std::filesystem::current_path());

        VERIFY_ARE_EQUAL(expected.wstring(), GetCanonicalPath(expected).wstring());
    }

    // A failure from std::filesystem::absolute must be reported. absolute returns an empty path when
    // it fails, and weakly_canonical succeeds on an empty path and clears the error_code, so calling
    // the two in sequence without checking in between silently turns the failure into success.
    TEST_METHOD(GetCanonicalPath_ErrorOverloadReportsFailure)
    {
        std::error_code error;
        const auto result = GetCanonicalPath(UnresolvablePath(), error);

        VERIFY_ARE_NOT_EQUAL(std::error_code{}, error);
        VERIFY_IS_TRUE(result.empty());
    }

    // Error must be cleared when the call succeeds so callers can reuse the same variable.
    TEST_METHOD(GetCanonicalPath_ErrorOverloadClearsErrorOnSuccess)
    {
        const auto expected = CanonicalCurrentDirectory() / UniqueMissingName(std::filesystem::current_path());

        auto error = std::make_error_code(std::errc::permission_denied);
        const auto result = GetCanonicalPath(expected, error);

        VERIFY_ARE_EQUAL(std::error_code{}, error);
        VERIFY_ARE_EQUAL(expected.wstring(), result.wstring());
    }

    // The throwing overload must surface the same failure the non-throwing overload reports.
    TEST_METHOD(GetCanonicalPath_ThrowingOverloadSurfacesFailure)
    {
        std::error_code error;
        (void)GetCanonicalPath(UnresolvablePath(), error);
        VERIFY_ARE_NOT_EQUAL(std::error_code{}, error);

        const auto expectedResult = HRESULT_FROM_WIN32(error.value());
        VERIFY_THROWS_SPECIFIC(GetCanonicalPath(UnresolvablePath()), wil::ResultException, [&](const wil::ResultException& e) {
            return e.GetErrorCode() == expectedResult;
        });
    }
};
} // namespace FilesystemUnitTests
