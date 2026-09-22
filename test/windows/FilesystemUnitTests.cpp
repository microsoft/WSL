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
using wsl::windows::common::filesystem::IsRepresentableFileName;
using wsl::windows::common::filesystem::MakeStagingDirectory;
using wsl::windows::common::filesystem::PosixBaseName;

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

    // The last component of a POSIX container path is what a copy is named after.
    TEST_METHOD(PosixBaseName_ReturnsLastComponent)
    {
        VERIFY_ARE_EQUAL(std::string{"thelink.txt"}, PosixBaseName("/tmp/linkdir/thelink.txt"));
        VERIFY_ARE_EQUAL(std::string{"file.txt"}, PosixBaseName("file.txt"));
        VERIFY_ARE_EQUAL(std::string{"tmp"}, PosixBaseName("/tmp"));
    }

    // A trailing separator names the same entry, so it cannot change the result.
    TEST_METHOD(PosixBaseName_IgnoresTrailingSeparators)
    {
        VERIFY_ARE_EQUAL(std::string{"dir"}, PosixBaseName("/tmp/dir/"));
        VERIFY_ARE_EQUAL(std::string{"dir"}, PosixBaseName("/tmp/dir///"));
    }

    // The root and the dot components carry no name of their own. A caller uses the empty result to fall
    // back to the names the archive already carries.
    TEST_METHOD(PosixBaseName_PathsWithNoNameYieldEmpty)
    {
        const char* const paths[] = {"/", "//", ".", "..", "/tmp/.", "/tmp/..", ""};

        for (const auto* path : paths)
        {
            VERIFY_ARE_EQUAL(std::string{}, PosixBaseName(path));
        }
    }

    // Only '/' and NUL are barred from a POSIX name, so a basename can hold characters that no Windows
    // file name can. The name is returned as it stands: judging it against a Windows destination belongs
    // to the caller, which otherwise could not tell a rejected name from a path that has no name at all.
    TEST_METHOD(PosixBaseName_KeepsNamesWindowsCannotRepresent)
    {
        const std::string colonName = "a:b";
        const std::string colonPath = "/tmp/a:b";
        VERIFY_ARE_EQUAL(colonName, PosixBaseName(colonPath));

        const std::string backslashName = "a\\b";
        const std::string backslashPath = "/tmp/a\\b";
        VERIFY_ARE_EQUAL(backslashName, PosixBaseName(backslashPath));

        const std::string wildcardName = "a*b";
        const std::string wildcardPath = "/tmp/a*b";
        VERIFY_ARE_EQUAL(wildcardName, PosixBaseName(wildcardPath));
    }

    // Staging goes under the parent it was given so that moving entries out of it afterwards stays on one
    // volume, and each directory has to be distinct so concurrent copies cannot collide.
    TEST_METHOD(MakeStagingDirectory_CreatesUniqueDirectoryUnderParent)
    {
        const auto parent = std::filesystem::current_path();

        const auto first = MakeStagingDirectory(parent);
        auto removeFirst = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(first, error);
        });

        VERIFY_IS_TRUE(std::filesystem::is_directory(first));
        VERIFY_ARE_EQUAL(parent.wstring(), first.parent_path().wstring());

        const auto second = MakeStagingDirectory(parent);
        auto removeSecond = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(second, error);
        });

        VERIFY_IS_TRUE(std::filesystem::is_directory(second));
        VERIFY_ARE_NOT_EQUAL(first.wstring(), second.wstring());
    }

    // A POSIX name bars only '/' and NUL, so a name taken from a container path can hold characters that
    // no Windows file name can. Those have to be caught before a path is built from them.
    TEST_METHOD(IsRepresentableFileName_AcceptsNamesWindowsCanCreate)
    {
        VERIFY_IS_TRUE(IsRepresentableFileName(L"thelink.txt"));
        VERIFY_IS_TRUE(IsRepresentableFileName(L"name with spaces"));
        VERIFY_IS_TRUE(IsRepresentableFileName(L"dots.in.name"));

        // An empty name stands for a path with no name of its own, which the caller handles separately.
        VERIFY_IS_TRUE(IsRepresentableFileName(L""));
    }

    TEST_METHOD(IsRepresentableFileName_RejectsReservedCharacters)
    {
        const std::wstring reserved[] = {L"a<b", L"a>b", L"a:b", L"a\"b", L"a/b", L"a\\b", L"a|b", L"a?b", L"a*b"};
        for (const auto& name : reserved)
        {
            VERIFY_IS_FALSE(IsRepresentableFileName(name), name.c_str());
        }
    }

    TEST_METHOD(IsRepresentableFileName_RejectsControlCharacters)
    {
        const std::wstring control = L"a\x01z";
        VERIFY_IS_FALSE(IsRepresentableFileName(control));

        const std::wstring newline = L"a\nz";
        VERIFY_IS_FALSE(IsRepresentableFileName(newline));
    }

    // The directory has to survive for the whole scope and be gone once it closes, since the copy that
    // uses it leaves entries behind that must not reach the destination.
    TEST_METHOD(StagingDirectory_RemovesItselfWhenScopeEnds)
    {
        std::filesystem::path recorded;
        {
            const wsl::windows::common::filesystem::StagingDirectory staging(std::filesystem::temp_directory_path());
            recorded = staging.Path();

            VERIFY_IS_TRUE(std::filesystem::is_directory(recorded));

            // Content under it goes away with it.
            std::ofstream file(recorded / L"entry.txt");
            file << "content";
            file.close();
            VERIFY_IS_TRUE(std::filesystem::exists(recorded / L"entry.txt"));
        }

        VERIFY_IS_FALSE(std::filesystem::exists(recorded));
    }
};
} // namespace FilesystemUnitTests
