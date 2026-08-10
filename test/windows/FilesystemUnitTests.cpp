/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    FilesystemUnitTests.cpp

Abstract:

    This file contains unit tests for the helpers in src/windows/common/filesystem.cpp.
    These tests only touch the local filesystem so they do not require an installed distribution.

--*/

#include "precomp.h"
#include "Common.h"

using wsl::windows::common::filesystem::GetCanonicalPath;

namespace {

// Creates a uniquely named directory under the temp directory and removes it on destruction.
class ScopedTempDirectory
{
public:
    ScopedTempDirectory()
    {
        m_path = std::filesystem::temp_directory_path() /
                 (L"wsl_ut_canonical_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++s_counter));
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    // The canonical form of the directory, which is what GetCanonicalPath is expected to resolve to.
    // N.B. std::filesystem::canonical is used rather than weakly_canonical so the expected value is
    // computed independently of the API under test.
    std::filesystem::path Canonical() const
    {
        return std::filesystem::canonical(m_path);
    }

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
    static inline int s_counter = 0;
};

// Sets the current directory for the lifetime of the object and restores the previous one on destruction.
class ScopedCurrentDirectory
{
public:
    explicit ScopedCurrentDirectory(const std::filesystem::path& Path) : m_previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(Path);
    }

    ~ScopedCurrentDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(m_previous, error);
    }

    ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
    ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

private:
    std::filesystem::path m_previous;
};

// Returns a file name that is guaranteed not to exist in the given directory.
std::wstring UniqueMissingName(const std::filesystem::path& Directory)
{
    const auto name = L"missing_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) + L".txt";
    VERIFY_IS_FALSE(std::filesystem::exists(Directory / name));
    return name;
}

} // namespace

namespace FilesystemUnitTests {
class FilesystemUnitTests
{
    WSL_TEST_CLASS(FilesystemUnitTests)

    // A relative path naming a file that does not exist must still resolve to an absolute path.
    // This is the case std::filesystem::weakly_canonical cannot handle on its own: it builds its result
    // from the longest leading sequence of elements that exist, so a bare missing file name has nothing
    // to canonicalize and is returned unchanged.
    TEST_METHOD(GetCanonicalPath_RelativeMissingPathIsMadeAbsolute)
    {
        ScopedTempDirectory directory;
        const auto name = UniqueMissingName(directory.Path());

        ScopedCurrentDirectory scopedDirectory(directory.Path());

        // Establish that the input is relative and that weakly_canonical alone leaves it that way.
        VERIFY_IS_FALSE(std::filesystem::path(name).is_absolute());
        VERIFY_IS_FALSE(std::filesystem::weakly_canonical(name).is_absolute());

        const auto result = GetCanonicalPath(name);
        VERIFY_IS_TRUE(result.is_absolute());
        VERIFY_ARE_EQUAL((directory.Canonical() / name).wstring(), result.wstring());
    }

    // The same resolution must happen for a relative path whose target already exists.
    TEST_METHOD(GetCanonicalPath_RelativeExistingPathIsMadeAbsolute)
    {
        ScopedTempDirectory directory;
        const std::wstring name = L"existing.txt";
        std::ofstream(directory.Path() / name).put('x');
        VERIFY_IS_TRUE(std::filesystem::exists(directory.Path() / name));

        ScopedCurrentDirectory scopedDirectory(directory.Path());

        const auto result = GetCanonicalPath(name);
        VERIFY_IS_TRUE(result.is_absolute());
        VERIFY_ARE_EQUAL((directory.Canonical() / name).wstring(), result.wstring());
    }

    // '.' and '..' components must be collapsed even when the intermediate directory does not exist.
    TEST_METHOD(GetCanonicalPath_CollapsesDotSegments)
    {
        ScopedTempDirectory directory;
        const auto name = UniqueMissingName(directory.Path());

        ScopedCurrentDirectory scopedDirectory(directory.Path());

        const auto result = GetCanonicalPath(L".\\nonexistent\\..\\" + name);
        VERIFY_ARE_EQUAL((directory.Canonical() / name).wstring(), result.wstring());
    }

    // An already absolute path must be returned unchanged.
    TEST_METHOD(GetCanonicalPath_AbsolutePathIsUnchanged)
    {
        ScopedTempDirectory directory;
        const auto expected = directory.Canonical() / UniqueMissingName(directory.Path());

        VERIFY_ARE_EQUAL(expected.wstring(), GetCanonicalPath(expected).wstring());
    }

    // A failure must be reported through Error rather than thrown.
    // N.B. An empty path is rejected by std::filesystem::absolute. This is the case the previous
    // weakly_canonical(absolute(Path, error), error) idiom silently dropped, because weakly_canonical
    // clears the error_code on success and therefore erased the failure absolute had just reported.
    TEST_METHOD(GetCanonicalPath_ErrorOverloadReportsFailure)
    {
        std::error_code error;
        const auto result = GetCanonicalPath(std::filesystem::path{}, error);

        VERIFY_IS_TRUE(!!error);
        VERIFY_IS_TRUE(result.empty());
    }

    // Error must be cleared when the call succeeds so callers can reuse the same variable.
    TEST_METHOD(GetCanonicalPath_ErrorOverloadClearsErrorOnSuccess)
    {
        ScopedTempDirectory directory;
        const auto expected = directory.Canonical() / UniqueMissingName(directory.Path());

        auto error = std::make_error_code(std::errc::permission_denied);
        const auto result = GetCanonicalPath(expected, error);

        VERIFY_IS_FALSE(!!error);
        VERIFY_ARE_EQUAL(expected.wstring(), result.wstring());
    }

    // The throwing overload must surface the same failure the non-throwing overload reports.
    TEST_METHOD(GetCanonicalPath_ThrowingOverloadSurfacesFailure)
    {
        std::error_code error;
        (void)GetCanonicalPath(std::filesystem::path{}, error);
        VERIFY_IS_TRUE(!!error);

        const auto expectedResult = HRESULT_FROM_WIN32(error.value());
        VERIFY_THROWS_SPECIFIC(GetCanonicalPath(std::filesystem::path{}), wil::ResultException, [&](const wil::ResultException& e) {
            return e.GetErrorCode() == expectedResult;
        });
    }
};
} // namespace FilesystemUnitTests