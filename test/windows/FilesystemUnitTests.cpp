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

using wsl::windows::common::filesystem::ExtractArchiveInto;
using wsl::windows::common::filesystem::ExtractSingleFileAs;
using wsl::windows::common::filesystem::GetCanonicalPath;
using wsl::windows::common::filesystem::IsRepresentableFileName;
using wsl::windows::common::filesystem::MakeStagingDirectory;
using wsl::windows::common::filesystem::PosixBaseName;
using wsl::windows::common::filesystem::StagingDirectory;

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

// Builds a ustar archive in memory. tar.exe is the extractor under test, so archives are assembled here
// rather than produced by tar, which keeps entry names, types and ordering under the test's control and
// allows archives that tar would not willingly create.
class TarBuilder
{
public:
    TarBuilder& AddFile(std::string_view Name, std::string_view Content)
    {
        AddEntry(Name, '0', Content, {});
        return *this;
    }

    TarBuilder& AddDirectory(std::string_view Name)
    {
        AddEntry(std::string{Name} + "/", '5', {}, {});
        return *this;
    }

    // An archive ends with two zero blocks. An archive with no entries is just those blocks.
    std::vector<char> Build() const
    {
        auto bytes = m_bytes;
        bytes.resize(bytes.size() + (2 * c_blockSize), '\0');
        return bytes;
    }

    // Matches the callback the extraction helpers invoke to obtain the archive.
    std::function<void(HANDLE)> Writer() const
    {
        return [bytes = Build()](HANDLE Handle) {
            // tar.exe can reject an entry and exit before the whole archive is written, which breaks the
            // pipe. That is already reported through its exit code, so a short write is left to surface
            // there rather than as a write failure here.
            DWORD written = 0;
            WriteFile(Handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        };
    }

private:
    static constexpr size_t c_blockSize = 512;

    void AddEntry(std::string_view Name, char TypeFlag, std::string_view Content, std::string_view LinkTarget)
    {
        const auto headerOffset = m_bytes.size();
        m_bytes.resize(headerOffset + c_blockSize, '\0');
        auto* header = m_bytes.data() + headerOffset;

        WriteText(header + 0, 100, Name);
        WriteOctal(header + 100, 8, TypeFlag == '5' ? 0755 : 0644);
        WriteOctal(header + 108, 8, 0);
        WriteOctal(header + 116, 8, 0);
        WriteOctal(header + 124, 12, Content.size());
        WriteOctal(header + 136, 12, 0);
        header[156] = TypeFlag;
        WriteText(header + 157, 100, LinkTarget);
        std::memcpy(header + 257, "ustar", 5);
        std::memcpy(header + 263, "00", 2);

        // The checksum covers the whole header with its own field read as spaces.
        std::memset(header + 148, ' ', 8);
        unsigned int sum = 0;
        for (size_t i = 0; i < c_blockSize; ++i)
        {
            sum += static_cast<unsigned char>(header[i]);
        }

        const auto checksum = std::format("{:06o}", sum);
        std::memcpy(header + 148, checksum.data(), checksum.size());
        header[154] = '\0';
        header[155] = ' ';

        // Content is padded out to a whole number of blocks.
        if (!Content.empty())
        {
            const auto padded = ((Content.size() + c_blockSize - 1) / c_blockSize) * c_blockSize;
            const auto contentOffset = m_bytes.size();
            m_bytes.resize(contentOffset + padded, '\0');
            std::memcpy(m_bytes.data() + contentOffset, Content.data(), Content.size());
        }
    }

    static void WriteText(char* Field, size_t Size, std::string_view Value)
    {
        VERIFY_IS_TRUE(Value.size() < Size);
        std::memcpy(Field, Value.data(), Value.size());
    }

    static void WriteOctal(char* Field, size_t Size, size_t Value)
    {
        const auto text = std::format("{:0{}o}", Value, Size - 1);
        VERIFY_IS_TRUE(text.size() < Size);
        std::memcpy(Field, text.data(), text.size());
    }

    std::vector<char> m_bytes;
};

std::string ReadFileContent(const std::filesystem::path& Path)
{
    std::ifstream file(Path, std::ios::binary);
    VERIFY_IS_TRUE(file.is_open());

    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteFileContent(const std::filesystem::path& Path, std::string_view Content)
{
    std::ofstream file(Path, std::ios::binary);
    VERIFY_IS_TRUE(file.is_open());
    file << Content;
}

size_t CountEntries(const std::filesystem::path& Directory)
{
    return static_cast<size_t>(std::distance(std::filesystem::directory_iterator(Directory), std::filesystem::directory_iterator{}));
}

// Extraction throws after the archive has been examined but before anything is moved into place. Every
// rejection reports E_FAIL, so the assertion covers the code alongside the fact that it threw.
void VerifyExtractionFails(const std::function<void()>& Extraction)
{
    VERIFY_THROWS_SPECIFIC(
        Extraction(), wil::ResultException, [](const wil::ResultException& e) { return e.GetErrorCode() == E_FAIL; });
}

// A name that cannot be addressed once it is in staging fails inside the move rather than through a
// validation check, so the code it carries is whatever Win32 reported.
void VerifyExtractionThrows(const std::function<void()>& Extraction)
{
    VERIFY_THROWS_SPECIFIC(Extraction(), wil::ResultException, [](const wil::ResultException&) { return true; });
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

    // Win32 strips a trailing space or dot, so accepting one would copy the entry under a name other
    // than the one the caller asked for.
    TEST_METHOD(IsRepresentableFileName_RejectsTrailingSpaceOrDot)
    {
        const std::wstring stripped[] = {L"file.", L"file ", L"file...", L"file   ", L".", L".."};
        for (const auto& name : stripped)
        {
            VERIFY_IS_FALSE(IsRepresentableFileName(name), name.c_str());
        }

        // A dot or space anywhere else survives untouched.
        VERIFY_IS_TRUE(IsRepresentableFileName(L".hidden"));
        VERIFY_IS_TRUE(IsRepresentableFileName(L"..two dots"));
    }

    TEST_METHOD(IsRepresentableFileName_RejectsReservedDeviceNames)
    {
        const std::wstring devices[] = {
            L"con", L"CON", L"Prn", L"aux", L"NUL", L"com1", L"COM9", L"lpt1", L"lpt9", L"LPT9", L"conin$", L"CONOUT$"};
        for (const auto& name : devices)
        {
            VERIFY_IS_FALSE(IsRepresentableFileName(name), name.c_str());
        }

        // A device name keeps its meaning when it carries an extension.
        VERIFY_IS_FALSE(IsRepresentableFileName(L"con.txt"));
        VERIFY_IS_FALSE(IsRepresentableFileName(L"NUL.tar.gz"));
    }

    TEST_METHOD(IsRepresentableFileName_AcceptsNamesThatOnlyLookReserved)
    {
        const std::wstring allowed[] = {L"con2", L"com0", L"lpt0", L"com10", L"console", L"nuls", L"prnt", L"auxiliary"};
        for (const auto& name : allowed)
        {
            VERIFY_IS_TRUE(IsRepresentableFileName(name), name.c_str());
        }
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

    // The destination name is supplied by the caller, so the name the entry carries inside the archive is
    // discarded. tar.exe cannot rename an entry while extracting, which is why the entry is staged first.
    TEST_METHOD(ExtractSingleFileAs_PlacesEntryUnderRequestedName)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"renamed.txt";

        ExtractSingleFileAs(destination, TarBuilder().AddFile("original.txt", "content").Writer());

        VERIFY_IS_TRUE(std::filesystem::is_regular_file(destination));
        VERIFY_ARE_EQUAL(std::string{"content"}, ReadFileContent(destination));
        VERIFY_IS_FALSE(std::filesystem::exists(root.Path() / L"original.txt"));
    }

    TEST_METHOD(ExtractSingleFileAs_PreservesEmptyFile)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"empty.txt";

        ExtractSingleFileAs(destination, TarBuilder().AddFile("source.txt", "").Writer());

        VERIFY_IS_TRUE(std::filesystem::is_regular_file(destination));
        VERIFY_ARE_EQUAL(std::string{}, ReadFileContent(destination));
    }

    // The whole leading path is created, so a copy does not have to be preceded by a mkdir.
    TEST_METHOD(ExtractSingleFileAs_CreatesMissingParentDirectories)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"missing" / L"nested" / L"file.txt";

        ExtractSingleFileAs(destination, TarBuilder().AddFile("source.txt", "content").Writer());

        VERIFY_ARE_EQUAL(std::string{"content"}, ReadFileContent(destination));
    }

    TEST_METHOD(ExtractSingleFileAs_OverwritesExistingDestination)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"file.txt";
        WriteFileContent(destination, "stale");

        ExtractSingleFileAs(destination, TarBuilder().AddFile("source.txt", "fresh").Writer());

        VERIFY_ARE_EQUAL(std::string{"fresh"}, ReadFileContent(destination));
    }

    // A file path names one entry, so an archive carrying nothing cannot satisfy it. Reporting this
    // matters because the alternative is to succeed having written no file at all.
    TEST_METHOD(ExtractSingleFileAs_EmptyArchiveIsRejected)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"file.txt";

        VerifyExtractionFails([&] { ExtractSingleFileAs(destination, TarBuilder().Writer()); });

        VERIFY_IS_FALSE(std::filesystem::exists(destination));
    }

    // Several entries cannot be named by one file path. Without this the entries would be silently
    // reduced to whichever one was moved last.
    TEST_METHOD(ExtractSingleFileAs_SeveralEntriesAreRejected)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"file.txt";

        VerifyExtractionFails([&] {
            ExtractSingleFileAs(destination, TarBuilder().AddFile("first.txt", "one").AddFile("second.txt", "two").Writer());
        });

        VERIFY_IS_FALSE(std::filesystem::exists(destination));
    }

    // A directory carries a tree, so giving it a file path would produce a directory named like a file.
    TEST_METHOD(ExtractSingleFileAs_DirectoryEntryIsRejected)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"file.txt";

        VerifyExtractionFails([&] { ExtractSingleFileAs(destination, TarBuilder().AddDirectory("adirectory").Writer()); });

        VERIFY_IS_FALSE(std::filesystem::exists(destination));
    }

    // A directory holding content is the same rejection, reached through the entry count rather than the
    // type of the single entry, since tar creates the parent directory for the entries beneath it.
    TEST_METHOD(ExtractSingleFileAs_PopulatedDirectoryIsRejected)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"file.txt";

        VerifyExtractionFails([&] {
            ExtractSingleFileAs(destination, TarBuilder().AddDirectory("adirectory").AddFile("adirectory/inner.txt", "content").Writer());
        });

        VERIFY_IS_FALSE(std::filesystem::exists(destination));
    }

    // A rejected archive must leave a file that is already there untouched, so a copy that cannot be
    // satisfied does not destroy the destination. The archive is examined only after extraction, so this
    // holds solely because extraction goes to staging rather than to the destination itself.
    TEST_METHOD(ExtractSingleFileAs_LeavesExistingDestinationIntactWhenRejected)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"file.txt";
        WriteFileContent(destination, "original");

        VerifyExtractionFails([&] {
            ExtractSingleFileAs(destination, TarBuilder().AddFile("first.txt", "one").AddFile("second.txt", "two").Writer());
        });

        VERIFY_ARE_EQUAL(std::string{"original"}, ReadFileContent(destination));
    }

    // A POSIX name can hold characters no Windows file name can. tar.exe replaces those characters, and
    // drops a leading element that reads as a drive letter, so the entry lands in staging under a name
    // Windows accepts. The destination name comes from the caller, so neither the original name nor the
    // replacement reaches the result. This is why no name check is needed here, unlike a copy onto a
    // directory, where the entry keeps its own name.
    TEST_METHOD(ExtractSingleFileAs_SanitizedEntryNameDoesNotReachTheResult)
    {
        const std::string names[] = {"a:b", "a*b", "a|b", "a<b", "a?b"};
        for (const auto& name : names)
        {
            const StagingDirectory root(std::filesystem::temp_directory_path());
            const auto destination = root.Path() / L"file.txt";

            ExtractSingleFileAs(destination, TarBuilder().AddFile(name, "content").Writer());

            VERIFY_IS_TRUE(std::filesystem::is_regular_file(destination));
            VERIFY_ARE_EQUAL(std::string{"content"}, ReadFileContent(destination));
        }
    }

    // A trailing dot or space, and a bare device name, survive extraction because tar.exe creates them
    // through a path form that bypasses the Win32 parsing rules. Nothing else can address them
    // afterwards, so the entry cannot be moved out of staging and the copy fails rather than producing a
    // file under some other name.
    TEST_METHOD(ExtractSingleFileAs_EntryNameWindowsCannotAddressFails)
    {
        const std::string names[] = {"trail.", "trail ", "nul"};
        for (const auto& name : names)
        {
            const StagingDirectory root(std::filesystem::temp_directory_path());
            const auto destination = root.Path() / L"file.txt";

            VerifyExtractionThrows([&] { ExtractSingleFileAs(destination, TarBuilder().AddFile(name, "content").Writer()); });

            VERIFY_IS_FALSE(std::filesystem::exists(destination));
        }
    }

    // An entry that climbs out of the directory it is extracted into is refused by tar.exe itself, which
    // keeps an archive from reaching a path the copy never named.
    TEST_METHOD(ExtractSingleFileAs_EntryEscapingTheDestinationIsRejected)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"file.txt";

        VerifyExtractionFails(
            [&] { ExtractSingleFileAs(destination, TarBuilder().AddFile("dir/../escaped.txt", "content").Writer()); });

        VERIFY_IS_FALSE(std::filesystem::exists(destination));
        VERIFY_IS_FALSE(std::filesystem::exists(root.Path() / L"escaped.txt"));
    }

    // Staging is an implementation detail that must not outlive the call, on either outcome, or a copy
    // would leave the destination directory littered.
    TEST_METHOD(ExtractSingleFileAs_RemovesStagingDirectory)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());

        ExtractSingleFileAs(root.Path() / L"file.txt", TarBuilder().AddFile("source.txt", "content").Writer());
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), CountEntries(root.Path()));

        VerifyExtractionFails([&] { ExtractSingleFileAs(root.Path() / L"other.txt", TarBuilder().Writer()); });
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), CountEntries(root.Path()));
    }

    // Without a rebase name the archive is extracted as it stands, which is what a copy onto a directory
    // that keeps the source names needs.
    TEST_METHOD(ExtractArchiveInto_WithoutRebaseNameKeepsEntryNames)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());

        ExtractArchiveInto(root.Path(), std::nullopt, TarBuilder().AddFile("first.txt", "one").AddFile("second.txt", "two").Writer());

        VERIFY_ARE_EQUAL(std::string{"one"}, ReadFileContent(root.Path() / L"first.txt"));
        VERIFY_ARE_EQUAL(std::string{"two"}, ReadFileContent(root.Path() / L"second.txt"));
    }

    // A lone entry is the source itself, so it takes the requested name.
    TEST_METHOD(ExtractArchiveInto_RebasesLoneEntry)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());

        ExtractArchiveInto(
            root.Path(), std::optional<std::wstring>{L"renamed.txt"}, TarBuilder().AddFile("original.txt", "content").Writer());

        VERIFY_ARE_EQUAL(std::string{"content"}, ReadFileContent(root.Path() / L"renamed.txt"));
        VERIFY_IS_FALSE(std::filesystem::exists(root.Path() / L"original.txt"));
    }

    // Several entries mean the source has no name of its own, so the name becomes a directory holding
    // them rather than being applied to any one of them.
    TEST_METHOD(ExtractArchiveInto_GathersSeveralEntriesUnderRebaseName)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());

        ExtractArchiveInto(
            root.Path(),
            std::optional<std::wstring>{L"gathered"},
            TarBuilder().AddFile("first.txt", "one").AddFile("second.txt", "two").Writer());

        VERIFY_IS_TRUE(std::filesystem::is_directory(root.Path() / L"gathered"));
        VERIFY_ARE_EQUAL(std::string{"one"}, ReadFileContent(root.Path() / L"gathered" / L"first.txt"));
        VERIFY_ARE_EQUAL(std::string{"two"}, ReadFileContent(root.Path() / L"gathered" / L"second.txt"));
    }

    // A set but empty name still stages, which merges the entries into the destination under their own
    // names instead of creating a directory named after nothing.
    TEST_METHOD(ExtractArchiveInto_EmptyRebaseNameMergesEntries)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());

        ExtractArchiveInto(
            root.Path(), std::optional<std::wstring>{L""}, TarBuilder().AddFile("first.txt", "one").AddFile("second.txt", "two").Writer());

        VERIFY_ARE_EQUAL(std::string{"one"}, ReadFileContent(root.Path() / L"first.txt"));
        VERIFY_ARE_EQUAL(std::string{"two"}, ReadFileContent(root.Path() / L"second.txt"));
    }

    TEST_METHOD(ExtractArchiveInto_CreatesMissingDestination)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());
        const auto destination = root.Path() / L"missing" / L"nested";

        ExtractArchiveInto(destination, std::nullopt, TarBuilder().AddFile("file.txt", "content").Writer());

        VERIFY_ARE_EQUAL(std::string{"content"}, ReadFileContent(destination / L"file.txt"));
    }

    // Staging must not survive the call, so the destination holds only what the archive carried.
    TEST_METHOD(ExtractArchiveInto_RemovesStagingDirectory)
    {
        const StagingDirectory root(std::filesystem::temp_directory_path());

        ExtractArchiveInto(
            root.Path(), std::optional<std::wstring>{L"renamed.txt"}, TarBuilder().AddFile("original.txt", "content").Writer());

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), CountEntries(root.Path()));
    }
};
} // namespace FilesystemUnitTests
