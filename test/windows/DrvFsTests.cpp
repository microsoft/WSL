/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DrvFsTests.cpp

Abstract:

    This file contains drvfs test cases.

--*/

#include "precomp.h"

#include "Common.h"
#include <AclAPI.h>
#include <fstream>
#include <filesystem>
#include "wslservice.h"
#include "registry.hpp"
#include "helpers.hpp"
#include "svccomm.hpp"
#include <userenv.h>
#include "Distribution.h"

#define LXSST_DRVFS_TEST_DIR L"C:\\drvfstest"
#define LXSST_DRVFS_RWX_TEST_FILE LXSST_DRVFS_TEST_DIR L"\\rwx"
#define LXSST_DRVFS_READONLY_TEST_FILE LXSST_DRVFS_TEST_DIR L"\\readonly"
#define LXSST_DRVFS_WRITEONLY_TEST_FILE LXSST_DRVFS_TEST_DIR L"\\writeonly"
#define LXSST_DRVFS_EXECUTEONLY_TEST_FILE LXSST_DRVFS_TEST_DIR L"\\executeonly"
#define LXSST_DRVFS_READONLYATTR_TEST_FILE LXSST_DRVFS_TEST_DIR L"\\readonlyattr"
#define LXSST_DRVFS_READONLYATTRDEL_TEST_FILE LXSST_DRVFS_TEST_DIR L"\\readonlyattrdel"
#define LXSST_DRVFS_EXECUTEONLY_TEST_DIR LXSST_DRVFS_TEST_DIR L"\\executeonlydir"
#define LXSST_DRVFS_EXECUTEONLY_TEST_DIR_CHILD LXSST_DRVFS_EXECUTEONLY_TEST_DIR L"\\child"
#define LXSST_DRVFS_READONLY_TEST_DIR LXSST_DRVFS_TEST_DIR L"\\noexecutedir"
#define LXSST_DRVFS_METADATA_TEST_DIR L"C:\\metadatatest"

#define LXSST_DRVFS_REPARSE_TEST_DIR L"C:\\reparsetest"

#define LXSST_DRVFS_SYMLINK_TEST_DIR L"C:\\symlink"

#define LXSST_DRVFS_METADATA_TEST_MODE (5)

#define LXSST_TESTS_INSTALL_COMMAND_LINE L"/bin/bash -c 'cd /data/test; ./build_tests.sh'"

#define LXSST_METADATA_EA_NAME_LENGTH (RTL_NUMBER_OF(LX_FILE_METADATA_UID_EA_NAME) - 1)

#define LX_DRVFS_DISABLE_NONE (0)
#define LX_DRVFS_DISABLE_QUERY_BY_NAME (1)
#define LX_DRVFS_DISABLE_QUERY_BY_NAME_AND_STAT_INFO (2)

using wsl::windows::common::wslutil::GetSystemErrorString;

namespace DrvFsTests {

class DrvFsTests
{
public:
    std::wstring SkipUnstableTestEnvVar =
        L"WSL_DISABLE_VB_UNSTABLE_TESTS=" + std::wstring{wsl::windows::common::helpers::IsWindows11OrAbove() ? L"0" : L"1"};

    void DrvFsCommon(int TestMode, std::optional<DrvFsMode> DrvFsMode = {}) const
    {
        auto cleanup = wil::scope_exit([TestMode] {
            RemoveDirectory(LXSST_DRVFS_REPARSE_TEST_DIR L"\\junction");
            RemoveDirectory(LXSST_DRVFS_REPARSE_TEST_DIR L"\\absolutelink");
            DeleteFileW(LXSST_DRVFS_REPARSE_TEST_DIR L"\\filelink");
            RemoveDirectory(LXSST_DRVFS_REPARSE_TEST_DIR L"\\relativelink");
            RemoveDirectory(LXSST_DRVFS_REPARSE_TEST_DIR L"\\test\\linktarget");
            DeleteFileW(LXSST_DRVFS_REPARSE_TEST_DIR L"\\test\\filetarget");
            RemoveDirectory(LXSST_DRVFS_REPARSE_TEST_DIR L"\\test");
            DeleteFileW(LXSST_DRVFS_REPARSE_TEST_DIR L"\\v1link");
            DeleteFileW(LXSST_DRVFS_REPARSE_TEST_DIR L"\\appexeclink");
            RemoveDirectory(LXSST_DRVFS_REPARSE_TEST_DIR);
            SetFileAttributes(LXSST_DRVFS_RWX_TEST_FILE, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(LXSST_DRVFS_RWX_TEST_FILE);
            DeleteFileW(LXSST_DRVFS_READONLY_TEST_FILE);
            DeleteFileW(LXSST_DRVFS_WRITEONLY_TEST_FILE);
            DeleteFileW(LXSST_DRVFS_EXECUTEONLY_TEST_FILE);
            DeleteFileW(LXSST_DRVFS_EXECUTEONLY_TEST_DIR_CHILD);
            SetFileAttributes(LXSST_DRVFS_READONLYATTR_TEST_FILE, FILE_ATTRIBUTE_NORMAL);

            DeleteFileW(LXSST_DRVFS_READONLYATTR_TEST_FILE);
            SetFileAttributes(LXSST_DRVFS_READONLYATTRDEL_TEST_FILE, FILE_ATTRIBUTE_NORMAL);

            DeleteFileW(LXSST_DRVFS_READONLYATTRDEL_TEST_FILE);
            RemoveDirectory(LXSST_DRVFS_EXECUTEONLY_TEST_DIR);
            RemoveDirectory(LXSST_DRVFS_READONLY_TEST_DIR);
            RemoveDirectory(LXSST_DRVFS_TEST_DIR);
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\file.txt");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR L"\\foo\uf03abar");
            RemoveDirectory(LXSST_DRVFS_SYMLINK_TEST_DIR "\\dir");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink1");
            RemoveDirectory(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink2");
            RemoveDirectory(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink3");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink4");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink5");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink6");
            RemoveDirectory(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink7");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink8");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink1");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink2");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink3");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink4");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink5");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink6");
            DeleteFileW(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink7");
            RemoveDirectory(LXSST_DRVFS_SYMLINK_TEST_DIR);
            if (TestMode == LXSST_DRVFS_METADATA_TEST_MODE)
            {
                DeleteFileW(LXSST_DRVFS_METADATA_TEST_DIR L"\\baduid");
                DeleteFileW(LXSST_DRVFS_METADATA_TEST_DIR L"\\badgid");
                DeleteFileW(LXSST_DRVFS_METADATA_TEST_DIR L"\\badmode");
                DeleteFileW(LXSST_DRVFS_METADATA_TEST_DIR L"\\badtype1");
                DeleteFileW(LXSST_DRVFS_METADATA_TEST_DIR L"\\badtype2");
                DeleteFileW(LXSST_DRVFS_METADATA_TEST_DIR L"\\nondevice");
                RemoveDirectory(LXSST_DRVFS_METADATA_TEST_DIR);
            }
        });

        VERIFY_NO_THROW(CreateDrvFsTestFiles(TestMode == LXSST_DRVFS_METADATA_TEST_MODE));
        std::wstringstream Command;

        Command << L"/bin/bash -c \"";
        Command << SkipUnstableTestEnvVar;
        Command << " /data/test/wsl_unit_tests drvfs -d $(wslpath '";
        Command << LxsstuGetLxssDirectory();
        Command << L"') -m ";
        Command << TestMode;
        Command << L"\"";
        std::wstringstream Logfile;
        Logfile << L"drvfs";
        Logfile << TestMode;
        VERIFY_NO_THROW(LxsstuRunTest(Command.str().c_str(), Logfile.str().c_str()));

        if (DrvFsMode.has_value() && DrvFsMode.value() == DrvFsMode::VirtioFs)
        {
            LogSkipped("TODO: debug test for virtiofs");
            return;
        }

        //
        // Check that the read-only attribute has been changed.
        //

        DWORD Attributes = GetFileAttributes(LXSST_DRVFS_READONLYATTR_TEST_FILE);
        DWORD Expected = FILE_ATTRIBUTE_NORMAL;
        VERIFY_ARE_EQUAL(Expected, Attributes);
        Attributes = GetFileAttributes(LXSST_DRVFS_RWX_TEST_FILE);
        Expected = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE;
        VERIFY_ARE_EQUAL(Expected, Attributes);

        //
        // Check that the second read-only file was deleted.
        //

        Expected = INVALID_FILE_ATTRIBUTES;
        Attributes = GetFileAttributes(LXSST_DRVFS_READONLYATTRDEL_TEST_FILE);
        VERIFY_ARE_EQUAL(Expected, Attributes);

        //
        // Check the NT symlinks.
        //

        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink1", L"file.txt", false));
        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink2", L"dir", true));
        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink3", L"..", true));
        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink4", L"..\\symlink\\file.txt", false));
        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink5", L"dir\\..\\file.txt", false));
        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink6", L"ntlink1", false));
        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink7", L"ntlink2", true));
        VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\ntlink8", L"foo\uf03abar", false));

        VERIFY_NO_THROW(VerifyDrvFsLxSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink1"));
        VERIFY_NO_THROW(VerifyDrvFsLxSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink2"));

        // Since target resolution is done on the Windows side in Plan 9, it is able to create an NT
        // link if the target path traverses an existing NT link (this is actually better than WSL 1).
        if (LxsstuVmMode())
        {
            VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink3", L"ntlink2\\..\\file.txt", false));
        }
        else
        {
            VERIFY_NO_THROW(VerifyDrvFsLxSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink3"));
        }

        VERIFY_NO_THROW(VerifyDrvFsLxSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink4"));
        VERIFY_NO_THROW(VerifyDrvFsLxSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink5"));
        VERIFY_NO_THROW(VerifyDrvFsLxSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink6"));

        // Plan 9 doesn't know about the Linux mount point on "dir", so it creates an NT link in this case.
        if (LxsstuVmMode())
        {
            VERIFY_NO_THROW(VerifyDrvFsSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink7", L"dir\\..\\file.txt", false));
        }
        else
        {
            VERIFY_NO_THROW(VerifyDrvFsLxSymlink(LXSST_DRVFS_SYMLINK_TEST_DIR "\\lxlink7"));
        }

        //
        // Check metadata is readable using Windows APIs.
        //

        if (TestMode == LXSST_DRVFS_METADATA_TEST_MODE)
        {
            VerifyDrvFsMetadata();
        }
    }

    static void VfsAccessDrvFs()
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests vfsaccess drvfs", L"vfsaccess_drvfs"));
    }

    static void FsCommonDrvFs()
    {
        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests fscommon drvfs", L"fscommon_drvfs"));
    }

    void DrvFs(DrvFsMode Mode)
    {
        SKIP_TEST_ARM64();

        VERIFY_NO_THROW(DrvFsCommon(LX_DRVFS_DISABLE_NONE, Mode));
    }

    void DrvFsFat() const
    {
        SKIP_TEST_ARM64();

        constexpr auto MountPoint = "C:\\lxss_fat";
        constexpr auto VhdPath = "C:\\lxss_fat.vhdx";
        auto Cleanup = wil::scope_exit([MountPoint, VhdPath] { DeleteVolume(MountPoint, VhdPath); });

        VERIFY_NO_THROW(CreateVolume("fat32", 100, MountPoint, VhdPath));
        VERIFY_NO_THROW(
            LxsstuRunTest((L"bash -c '" + SkipUnstableTestEnvVar + L" /data/test/wsl_unit_tests drvfs -m 3'").c_str(), L"drvfs3"));
    }

    void DrvFsSmb() const
    {
        SKIP_TEST_ARM64();

        VERIFY_NO_THROW(
            LxsstuRunTest((L"bash -c '" + SkipUnstableTestEnvVar + L" /data/test/wsl_unit_tests drvfs -m 4'").c_str(), L"drvfs4"));
    }

    void DrvFsMetadata(DrvFsMode Mode)
    {
        SKIP_TEST_ARM64();

        VERIFY_NO_THROW(DrvFsCommon(LXSST_DRVFS_METADATA_TEST_MODE, Mode));
    }

    void DrvfsMountElevated(DrvFsMode Mode)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY(); // TODO: Enable on Windows 10 when virtio support is added
        SKIP_TEST_ARM64();

        TerminateDistribution();
        WslKeepAlive keelAlive;

        ValidateDrvfsMounts(CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, Mode);
    }

    void DrvfsMountElevatedDifferentConsole(DrvFsMode Mode)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY(); // TODO: Enable on Windows 10 when virtio support is added
        SKIP_TEST_ARM64();

        TerminateDistribution();
        WslKeepAlive keelAlive;

        ValidateDrvfsMounts(CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_CONSOLE, Mode);
    }

    void DrvfsMountNonElevated(DrvFsMode Mode)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY(); // TODO: Enable on Windows 10 when virtio support is added
        SKIP_TEST_ARM64();

        TerminateDistribution();

        const auto nonElevatedToken = GetNonElevatedToken();
        WslKeepAlive keelAlive(nonElevatedToken.get());

        ValidateDrvfsMounts(CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, Mode);
    }

    void DrvfsMountNonElevatedDifferentConsole(DrvFsMode Mode)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY(); // TODO: Enable on Windows 10 when virtio support is added
        SKIP_TEST_ARM64();

        TerminateDistribution();

        const auto nonElevatedToken = GetNonElevatedToken();
        WslKeepAlive keelAlive(nonElevatedToken.get());

        ValidateDrvfsMounts(CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_CONSOLE, Mode);
    }

    static void XattrDrvFs(DrvFsMode Mode)
    {
        SKIP_TEST_ARM64();

        if (Mode == DrvFsMode::VirtioFs)
        {
            LogSkipped("TODO: debug test for virtiofs");
            return;
        }

        VERIFY_NO_THROW(LxsstuRunTest(L"/data/test/wsl_unit_tests xattr drvfs", L"xattr_drvfs"));
    }

    void DrvFsReFs() const
    {
        SKIP_TEST_ARM64();
        WSL_TEST_VERSION_REQUIRED(wsl::windows::common::helpers::WindowsBuildNumbers::Germanium);

        constexpr auto MountPoint = "C:\\lxss_refs";
        constexpr auto VhdPath = "C:\\lxss_refs.vhdx";
        auto Cleanup = wil::scope_exit([MountPoint, VhdPath] { DeleteVolume(MountPoint, VhdPath); });

        VERIFY_NO_THROW(CreateVolume("refs", 50000, MountPoint, VhdPath));
        VERIFY_NO_THROW(
            LxsstuRunTest((L"bash -c '" + SkipUnstableTestEnvVar + L" /data/test/wsl_unit_tests drvfs -m 6'").c_str(), L"drvfs6"));
    }

    // DrvFsTests Private Methods
private:
    static VOID CreateDrvFsTestFiles(bool Metadata)
    {

        THROW_LAST_ERROR_IF(!CreateDirectory(LXSST_DRVFS_TEST_DIR, NULL));

        //
        // The rwx and readonlyattr test files need read/write EA permission for
        // the metadata test mode because chmod will be called on them.
        //

        CreateTestFile(LXSST_DRVFS_RWX_TEST_FILE, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_EXECUTE | DELETE | SYNCHRONIZE, FALSE, INVALID_HANDLE_VALUE);

        CreateTestFile(LXSST_DRVFS_READONLY_TEST_FILE, FILE_GENERIC_READ | DELETE | SYNCHRONIZE, FALSE, INVALID_HANDLE_VALUE);

        CreateTestFile(LXSST_DRVFS_WRITEONLY_TEST_FILE, FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES | FILE_READ_EA | DELETE | SYNCHRONIZE, FALSE, INVALID_HANDLE_VALUE);

        CreateTestFile(
            LXSST_DRVFS_EXECUTEONLY_TEST_DIR,
            FILE_TRAVERSE | FILE_DELETE_CHILD | FILE_ADD_FILE | FILE_READ_ATTRIBUTES | FILE_READ_EA | DELETE | SYNCHRONIZE | READ_CONTROL,
            TRUE,
            INVALID_HANDLE_VALUE);

        CreateTestFile(LXSST_DRVFS_EXECUTEONLY_TEST_DIR_CHILD, FILE_GENERIC_READ | DELETE | SYNCHRONIZE, FALSE, INVALID_HANDLE_VALUE);

        CreateTestFile(LXSST_DRVFS_READONLY_TEST_DIR, FILE_GENERIC_READ | DELETE | SYNCHRONIZE, TRUE, INVALID_HANDLE_VALUE);

        CreateTestFile(LXSST_DRVFS_READONLYATTR_TEST_FILE, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_EXECUTE | DELETE | SYNCHRONIZE, FALSE, INVALID_HANDLE_VALUE);

        THROW_LAST_ERROR_IF(!SetFileAttributes(LXSST_DRVFS_READONLYATTR_TEST_FILE, FILE_ATTRIBUTE_READONLY));

        CreateTestFile(LXSST_DRVFS_READONLYATTRDEL_TEST_FILE, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_EXECUTE | DELETE | SYNCHRONIZE, FALSE, INVALID_HANDLE_VALUE);

        THROW_LAST_ERROR_IF(!SetFileAttributes(LXSST_DRVFS_READONLYATTRDEL_TEST_FILE, FILE_ATTRIBUTE_READONLY));

        //
        // Copy the wsl_unit_tests executable to an execute-only file on DrvFs.
        //

        const std::wstring Path = L"\\\\wsl.localhost\\" LXSS_DISTRO_NAME_TEST_L L"\\data\\test\\wsl_unit_tests";
        const wil::unique_hfile File(CreateFile(
            Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));

        THROW_LAST_ERROR_IF(!File);
        CreateTestFile(
            LXSST_DRVFS_EXECUTEONLY_TEST_FILE,
            FILE_EXECUTE | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA | DELETE | SYNCHRONIZE | READ_CONTROL,
            FALSE,
            File.get());

        THROW_LAST_ERROR_IF(!CreateDirectory(LXSST_DRVFS_REPARSE_TEST_DIR, nullptr));

        THROW_LAST_ERROR_IF(!CreateDirectory(LXSST_DRVFS_REPARSE_TEST_DIR L"\\test", nullptr));

        THROW_LAST_ERROR_IF(!CreateDirectory(LXSST_DRVFS_REPARSE_TEST_DIR L"\\test\\linktarget", nullptr));

        THROW_LAST_ERROR_IF(!CreateSymbolicLink(
            LXSST_DRVFS_REPARSE_TEST_DIR L"\\absolutelink", LXSST_DRVFS_REPARSE_TEST_DIR L"\\test\\linktarget", SYMBOLIC_LINK_FLAG_DIRECTORY));

        THROW_LAST_ERROR_IF(!CreateSymbolicLink(LXSST_DRVFS_REPARSE_TEST_DIR L"\\relativelink", L"test\\linktarget", SYMBOLIC_LINK_FLAG_DIRECTORY));

        {
            const wil::unique_hfile TargetFile(CreateFile(
                LXSST_DRVFS_REPARSE_TEST_DIR L"\\test\\filetarget",
                FILE_GENERIC_READ | FILE_GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));

            THROW_LAST_ERROR_IF(!TargetFile);
        }

        THROW_LAST_ERROR_IF(!CreateSymbolicLink(LXSST_DRVFS_REPARSE_TEST_DIR L"\\filelink", L"test\\filetarget", 0));

        CreateJunction(LXSST_DRVFS_REPARSE_TEST_DIR L"\\junction", LXSST_DRVFS_REPARSE_TEST_DIR L"\\test\\linktarget");

        // DrvFs does not create V1 symlinks anymore; create one here manually to ensure it can still
        // read them.
        CreateV1Symlink(LXSST_DRVFS_REPARSE_TEST_DIR L"\\v1link", "/v1/symlink/target");
        CreateAppExecLink(LXSST_DRVFS_REPARSE_TEST_DIR L"\\appexeclink");

        if (Metadata != false)
        {
            THROW_LAST_ERROR_IF(!CreateDirectory(LXSST_DRVFS_METADATA_TEST_DIR, nullptr));

            CreateMetadataTestFile(LXSST_DRVFS_METADATA_TEST_DIR "\\baduid", LX_UID_INVALID, 3001, LX_S_IFREG | 0644, 0, 0, false);

            CreateMetadataTestFile(LXSST_DRVFS_METADATA_TEST_DIR "\\badgid", 3000, LX_GID_INVALID, LX_S_IFREG | 0644, 0, 0, false);

            CreateMetadataTestFile(LXSST_DRVFS_METADATA_TEST_DIR "\\badmode", 3000, 3001, 0x10000 | LX_S_IFREG | 0644, 0, 0, false);

            CreateMetadataTestFile(LXSST_DRVFS_METADATA_TEST_DIR "\\badtype1", 3000, 3001, LX_S_IFDIR | 0755, 0, 0, false);

            CreateMetadataTestFile(LXSST_DRVFS_METADATA_TEST_DIR "\\badtype2", 3000, 3001, LX_S_IFLNK | 0777, 0, 0, false);

            CreateMetadataTestFile(LXSST_DRVFS_METADATA_TEST_DIR "\\nondevice", 3000, 3001, LX_S_IFREG | 0644, 1, 2, true);
        }
    }

    static VOID CreateTestFile(_In_z_ LPCWSTR Filename, _In_ DWORD Permissions, _In_ BOOLEAN Directory, _In_ HANDLE SourceFile)
    {

        BYTE Buffer[4096];
        DWORD BytesRead;

        //
        // Create the SID for the BUILTIN\Administrators group.
        //

        auto [AdminSid, SidBuffer] =
            wsl::windows::common::security::CreateSid(SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

        //
        // Set the permissions for the SID.
        //

        EXPLICIT_ACCESS Access;
        RtlZeroMemory(&Access, sizeof(Access));
        Access.grfAccessPermissions = Permissions;
        Access.grfAccessMode = SET_ACCESS;
        Access.grfInheritance = NO_INHERITANCE;
        Access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        Access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        Access.Trustee.ptstrName = (LPTSTR)AdminSid;

        //
        // Allocate an ACL with the permissions.
        //

        wil::unique_any<PACL, decltype(&::LocalFree), ::LocalFree> Acl;
        THROW_IF_WIN32_ERROR(SetEntriesInAcl(1, &Access, NULL, &Acl));

        //
        // Create a security descriptor and set the ACL.
        //

        const wil::unique_hlocal_security_descriptor Descriptor(::LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));

        THROW_LAST_ERROR_IF(!Descriptor);
        THROW_LAST_ERROR_IF(!InitializeSecurityDescriptor(Descriptor.get(), SECURITY_DESCRIPTOR_REVISION));

        THROW_LAST_ERROR_IF(!SetSecurityDescriptorDacl(Descriptor.get(), TRUE, Acl.get(), FALSE));

        //
        // Create security attributes that point to the descriptor.
        //

        SECURITY_ATTRIBUTES Attributes;
        RtlZeroMemory(&Attributes, sizeof(Attributes));
        Attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        Attributes.lpSecurityDescriptor = Descriptor.get();
        Attributes.bInheritHandle = FALSE;

        //
        // Create a file or directory with the security attributes.
        //

        if (Directory == FALSE)
        {
            const wil::unique_hfile File(
                CreateFile(Filename, GENERIC_WRITE | SYNCHRONIZE, 0, &Attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL));

            THROW_LAST_ERROR_IF(!File);

            //
            // If a source file was specified, copy its contents.
            //

            if (SourceFile != INVALID_HANDLE_VALUE)
            {
                THROW_LAST_ERROR_IF(!ReadFile(SourceFile, Buffer, sizeof(Buffer), &BytesRead, NULL));

                while (BytesRead > 0)
                {
                    THROW_LAST_ERROR_IF(!WriteFile(File.get(), Buffer, BytesRead, NULL, NULL));

                    THROW_LAST_ERROR_IF(!ReadFile(SourceFile, Buffer, sizeof(Buffer), &BytesRead, NULL));
                }
            }
        }
        else
        {
            THROW_LAST_ERROR_IF(!CreateDirectory(Filename, &Attributes));
        }
    }

    static VOID CreateMetadataTestFile(
        _In_ LPCWSTR Filename, _In_ ULONG Uid, _In_ ULONG Gid, _In_ ULONG Mode, _In_ ULONG DeviceIdMajor, _In_ ULONG DeviceIdMinor, _In_ bool IncludeDeviceId)
    {

        //
        // Each individual EA entry must be aligned on a 4 byte boundary, but the
        // value inside each EA struct must not be. Therefore, set packing to 1
        // byte, and add padding to manually align the entries.
        //

#pragma pack(push, 1)

        struct
        {
            struct
            {
                union
                {
                    FILE_FULL_EA_INFORMATION Header;
                    CHAR Buffer[FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + LXSST_METADATA_EA_NAME_LENGTH + 1];
                };

                ULONG Uid;
            } Uid;

            CHAR Padding1;
            struct
            {
                union
                {
                    FILE_FULL_EA_INFORMATION Header;
                    CHAR Buffer[FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + LXSST_METADATA_EA_NAME_LENGTH + 1];
                };

                ULONG Gid;
            } Gid;

            CHAR Padding2;
            struct
            {
                union
                {
                    FILE_FULL_EA_INFORMATION Header;
                    CHAR Buffer[FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + LXSST_METADATA_EA_NAME_LENGTH + 1];
                };

                ULONG Mode;
            } Mode;

            CHAR Padding3;
            struct
            {
                union
                {
                    FILE_FULL_EA_INFORMATION Header;
                    CHAR Buffer[FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + LXSST_METADATA_EA_NAME_LENGTH + 1];
                };

                ULONG DeviceIdMajor;
                ULONG DeviceIdMinor;
            } DeviceId;
        } EaBuffer;

#pragma pack(pop)

        RtlZeroMemory(&EaBuffer, sizeof(EaBuffer));
        EaBuffer.Uid.Header.EaNameLength = LXSST_METADATA_EA_NAME_LENGTH;
        EaBuffer.Uid.Header.EaValueLength = sizeof(ULONG);
        RtlCopyMemory(EaBuffer.Uid.Header.EaName, LX_FILE_METADATA_UID_EA_NAME, LXSST_METADATA_EA_NAME_LENGTH);

        EaBuffer.Uid.Uid = Uid;
        EaBuffer.Uid.Header.NextEntryOffset = (ULONG)((PUCHAR)&EaBuffer.Gid - (PUCHAR)&EaBuffer.Uid);
        EaBuffer.Gid.Header.EaNameLength = LXSST_METADATA_EA_NAME_LENGTH;
        EaBuffer.Gid.Header.EaValueLength = sizeof(ULONG);
        RtlCopyMemory(EaBuffer.Gid.Header.EaName, LX_FILE_METADATA_GID_EA_NAME, LXSST_METADATA_EA_NAME_LENGTH);

        EaBuffer.Gid.Gid = Gid;
        EaBuffer.Gid.Header.NextEntryOffset = (ULONG)((PUCHAR)&EaBuffer.Mode - (PUCHAR)&EaBuffer.Gid);
        EaBuffer.Mode.Header.EaNameLength = LXSST_METADATA_EA_NAME_LENGTH;
        EaBuffer.Mode.Header.EaValueLength = sizeof(ULONG);
        RtlCopyMemory(EaBuffer.Mode.Header.EaName, LX_FILE_METADATA_MODE_EA_NAME, LXSST_METADATA_EA_NAME_LENGTH);

        EaBuffer.Mode.Mode = Mode;
        if (IncludeDeviceId != false)
        {
            EaBuffer.Mode.Header.NextEntryOffset = (ULONG)((PUCHAR)&EaBuffer.DeviceId - (PUCHAR)&EaBuffer.Mode);
            EaBuffer.DeviceId.Header.EaNameLength = LXSST_METADATA_EA_NAME_LENGTH;
            EaBuffer.DeviceId.Header.EaValueLength = sizeof(ULONG);
            RtlCopyMemory(EaBuffer.DeviceId.Header.EaName, LX_FILE_METADATA_DEVICE_ID_EA_NAME, LXSST_METADATA_EA_NAME_LENGTH);

            EaBuffer.DeviceId.DeviceIdMajor = DeviceIdMajor;
            EaBuffer.DeviceId.DeviceIdMinor = DeviceIdMinor;
        }

        const std::wstring NtPath{std::wstring(L"\\DosDevices\\") + Filename};
        UNICODE_STRING Name;
        RtlInitUnicodeString(&Name, NtPath.c_str());
        OBJECT_ATTRIBUTES Attributes;
        InitializeObjectAttributes(&Attributes, &Name, 0, nullptr, 0);
        wil::unique_hfile File;
        IO_STATUS_BLOCK IoStatus;
        THROW_IF_NTSTATUS_FAILED(NtCreateFile(
            &File,
            FILE_GENERIC_READ,
            &Attributes,
            &IoStatus,
            nullptr,
            FILE_ATTRIBUTE_NORMAL,
            (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
            FILE_CREATE,
            0,
            &EaBuffer,
            sizeof(EaBuffer)));
    }

    static VOID VerifyDrvFsMetadata()
    {
        wil::unique_hfile File;
        IO_STATUS_BLOCK IoStatus;
        UNICODE_STRING Name;
        RtlInitUnicodeString(&Name, L"\\DosDevices\\" LXSST_DRVFS_METADATA_TEST_DIR);
        OBJECT_ATTRIBUTES Attributes;
        InitializeObjectAttributes(&Attributes, &Name, 0, nullptr, 0);
        THROW_IF_NTSTATUS_FAILED(NtCreateFile(
            &File, FILE_READ_EA, &Attributes, &IoStatus, nullptr, 0, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), FILE_OPEN, FILE_DIRECTORY_FILE, nullptr, 0));

        UCHAR Buffer[1000];
        THROW_IF_NTSTATUS_FAILED(ZwQueryEaFile(File.get(), &IoStatus, Buffer, sizeof(Buffer), FALSE, nullptr, 0, nullptr, TRUE));

        bool FoundUid = false;
        bool FoundGid = false;
        bool FoundMode = false;
        PFILE_FULL_EA_INFORMATION EaInfo = (PFILE_FULL_EA_INFORMATION)Buffer;
        for (;;)
        {
            VERIFY_ARE_EQUAL(EaInfo->EaNameLength, 6);
            VERIFY_ARE_EQUAL(EaInfo->EaValueLength, 4);
            std::string EaName{EaInfo->EaName};
            ULONG Value = *(PULONG)((PCHAR)EaInfo->EaName + EaInfo->EaNameLength + 1);
            if (EaName == LX_FILE_METADATA_UID_EA_NAME)
            {
                FoundUid = true;
                VERIFY_ARE_EQUAL(Value, 0x11223344ul);
            }
            else if (EaName == LX_FILE_METADATA_GID_EA_NAME)
            {
                FoundGid = true;
                VERIFY_ARE_EQUAL(Value, 0x55667788ul);
            }
            else if (EaName == LX_FILE_METADATA_MODE_EA_NAME)
            {
                FoundMode = true;
                VERIFY_ARE_EQUAL(Value, (ULONG)(LX_S_IFDIR | 0775));
            }
            else
            {
                VERIFY_FAIL(L"Unexpected EA on file.");
            }

            if (EaInfo->NextEntryOffset == 0)
            {
                break;
            }

            EaInfo = (PFILE_FULL_EA_INFORMATION)((PCHAR)EaInfo + EaInfo->NextEntryOffset);
        };

        VERIFY_IS_TRUE(FoundUid);
        VERIFY_IS_TRUE(FoundGid);
        VERIFY_IS_TRUE(FoundMode);
    }

    static VOID CreateJunction(_In_ const std::wstring& Junction, _In_ const std::wstring& Target)
    {

        //
        // The logic for creating a junction was taken from mklink.
        //

        THROW_LAST_ERROR_IF(!CreateDirectory(Junction.c_str(), NULL));
        const wil::unique_hfile Dir(CreateFile(
            Junction.c_str(), FILE_GENERIC_WRITE, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));

        THROW_LAST_ERROR_IF(!Dir);

        UNICODE_STRING LinkPath = {0};
        auto Cleanup = wil::scope_exit([&] {
            if (LinkPath.Buffer != nullptr)
            {
                FREE(LinkPath.Buffer);
            }
        });

        THROW_IF_NTSTATUS_FAILED(RtlDosPathNameToNtPathName_U_WithStatus(Target.c_str(), &LinkPath, nullptr, nullptr));

        //
        // The buffer needs space for the substitute name and the print name, with
        // NULL characters. This can't overflow since they are all paths with
        // lengths less than MAXUSHORT.
        //

        const ULONG ReparseBufferSize = (ULONG)(FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer[0]) +
                                                Target.length() * sizeof(WCHAR) + LinkPath.Length + 2 * sizeof(UNICODE_NULL));

        //
        // Allocate the reparse data buffer.
        //

        const std::unique_ptr<REPARSE_DATA_BUFFER> Reparse((PREPARSE_DATA_BUFFER) new char[ReparseBufferSize]);

        ZeroMemory(Reparse.get(), ReparseBufferSize);
        Reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;

        //
        // The data length is the buffer size excluding the header.
        //

        Reparse->ReparseDataLength = (USHORT)(ReparseBufferSize - REPARSE_DATA_BUFFER_HEADER_SIZE);

        //
        // Copy the NT path into the buffer for the substitute name.
        //

        Reparse->MountPointReparseBuffer.SubstituteNameLength = LinkPath.Length;
        RtlCopyMemory(Reparse->MountPointReparseBuffer.PathBuffer, LinkPath.Buffer, LinkPath.Length);

        const USHORT Offset = LinkPath.Length + sizeof(UNICODE_NULL);

        //
        // Copy the DOS path into the buffer for the print name.
        //

        Reparse->MountPointReparseBuffer.PrintNameOffset = Offset;
        Reparse->MountPointReparseBuffer.PrintNameLength = (USHORT)(Target.length() * sizeof(WCHAR));

        RtlCopyMemory((Reparse->MountPointReparseBuffer.PathBuffer + (Offset / sizeof(WCHAR))), Target.c_str(), Target.length() * sizeof(WCHAR));

        //
        // Set the reparse point on the file.
        //

        IO_STATUS_BLOCK IoStatus;
        THROW_IF_NTSTATUS_FAILED(NtFsControlFile(
            Dir.get(), nullptr, nullptr, nullptr, &IoStatus, FSCTL_SET_REPARSE_POINT, Reparse.get(), ReparseBufferSize, nullptr, 0));

        return;
    }

    static VOID CreateV1Symlink(const std::wstring& Symlink, std::string_view Target)
    {

        //
        // Create a symlink using the V1 LX symlink format, where the target is
        // stored in the file data. The reparse data only contains a version
        // number.
        //

        union
        {
            REPARSE_DATA_BUFFER Header;
            struct
            {
                CHAR Buffer[REPARSE_DATA_BUFFER_HEADER_SIZE];
                ULONG Version;
            } Data;
        } Reparse{};

        constexpr ULONG ReparseBufferSize = REPARSE_DATA_BUFFER_HEADER_SIZE + sizeof(ULONG);

        //
        // The data length is the buffer size excluding the header.
        //

        Reparse.Header.ReparseTag = IO_REPARSE_TAG_LX_SYMLINK;
        Reparse.Header.ReparseDataLength = sizeof(ULONG);
        Reparse.Data.Version = 1;

        const auto File = CreateReparsePoint(Symlink, &Reparse, ReparseBufferSize);

        //
        // Write the target to the file.
        //

        DWORD written;
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(File.get(), Target.data(), gsl::narrow_cast<DWORD>(Target.size()), &written, nullptr));
        VERIFY_ARE_EQUAL(Target.size(), written);

        return;
    }

    static VOID CreateAppExecLink(const std::wstring& Link)
    {
        //
        // This link will not be valid from Windows's perspective, since it only
        // contains the header and not any actual reparse data. However, it has the
        // right reparse tag which is sufficient to test drvfs's behavior.
        //

        REPARSE_DATA_BUFFER Reparse{};
        Reparse.ReparseTag = IO_REPARSE_TAG_APPEXECLINK;
        Reparse.ReparseDataLength = 0;
        CreateReparsePoint(Link, &Reparse, REPARSE_DATA_BUFFER_HEADER_SIZE);
    }

    static wil::unique_hfile CreateReparsePoint(_In_ const std::wstring& Path, _In_ PVOID ReparseBuffer, _In_ ULONG ReparseBufferSize)
    {
        wil::unique_hfile File{CreateFile(
            Path.c_str(), FILE_GENERIC_WRITE, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};

        THROW_LAST_ERROR_IF(!File);
        IO_STATUS_BLOCK IoStatus;
        THROW_IF_NTSTATUS_FAILED(NtFsControlFile(
            File.get(), nullptr, nullptr, nullptr, &IoStatus, FSCTL_SET_REPARSE_POINT, ReparseBuffer, ReparseBufferSize, nullptr, 0));

        return File;
    }

    static VOID CreateVolume(LPCSTR FileSystem, ULONG MaxSizeInMb, LPCSTR MountPoint, LPCSTR VhdPath)
    {
        THROW_LAST_ERROR_IF(!CreateDirectoryA(MountPoint, NULL));

        const auto CreateScript = std::vformat(
            "create vdisk file={} maximum={} type=expandable\n"
            "select vdisk file={}\n"
            "attach vdisk\n"
            "create partition primary\n"
            "select partition 1\n"
            "online volume\n"
            "format fs={} quick\n"
            "assign mount={}\n",
            std::make_format_args(VhdPath, MaxSizeInMb, VhdPath, FileSystem, MountPoint));

        RunDiskpartScript(CreateScript.c_str());
    }

    static VOID RunDiskpartScript(LPCSTR Script)
    {
        const std::wstring ScriptFileName = wsl::windows::common::filesystem::GetTempFilename();

        std::ofstream ScriptFile(ScriptFileName);
        THROW_LAST_ERROR_IF(!ScriptFile);

        auto Cleanup = wil::scope_exit([&] { DeleteFileW(ScriptFileName.c_str()); });

        ScriptFile << Script;
        ScriptFile.close();

        std::wstring CommandLine = L"diskpart.exe /s " + ScriptFileName;
        THROW_HR_IF(E_FAIL, ((wsl::windows::common::helpers::RunProcess(CommandLine)) != 0));
    }

    static VOID DeleteVolume(LPCSTR MountPoint, LPCSTR VhdPath)
    {
        const auto CleanupScript = std::vformat(
            "select vdisk file={}\n"
            "select partition 1\n"
            "remove all\n"
            "detach vdisk\n",
            std::make_format_args(VhdPath));

        RunDiskpartScript(CleanupScript.c_str());

        RemoveDirectoryA(MountPoint);
        DeleteFileA(VhdPath);
    }

    static void ValidateDrvfsMounts(DWORD CreateProcessFlags, DrvFsMode Mode)
    {
        auto validate = [CreateProcessFlags](const std::wstring& expectedType, HANDLE token) {
            const auto commandLine = LxssGenerateWslCommandLine(L"mount | grep -F '/mnt/c type'");

            wsl::windows::common::SubProcess process(nullptr, commandLine.c_str(), CreateProcessFlags);
            process.SetToken(token);
            process.SetShowWindow(SW_HIDE);

            const auto output = process.RunAndCaptureOutput();
            const auto lines = LxssSplitString(output.Stdout, L"\n");

            VERIFY_ARE_EQUAL(lines.size(), 1);
            VERIFY_IS_TRUE(output.Stdout.find(expectedType) == 0);
        };

        std::wstring elevatedType;
        std::wstring nonElevatedType;
        switch (Mode)
        {
        case DrvFsMode::Plan9:
            elevatedType = L"C:\\";
            nonElevatedType = L"C:\\";
            break;
        case DrvFsMode::Virtio9p:
            elevatedType = L"drvfsa";
            nonElevatedType = L"drvfs";
            break;
        case DrvFsMode::VirtioFs:
            elevatedType = L"drvfsaC";
            nonElevatedType = L"drvfsC";
            break;

        default:
            LogError("Unexpected mode %d", (int)Mode);
            return;
        }

        // Validate that mount types are correct in both namespaces
        validate(elevatedType, nullptr);

        const auto nonElevatedToken = GetNonElevatedToken();
        validate(nonElevatedType, nonElevatedToken.get());
    }

    static VOID VerifyDrvFsSymlink(const std::wstring& Path, const std::wstring& ExpectedTarget, bool Directory)
    {

        const std::wstring NtPath = L"\\DosDevices\\" + Path;
        UNICODE_STRING Name;
        RtlInitUnicodeString(&Name, NtPath.c_str());
        OBJECT_ATTRIBUTES Attributes;
        InitializeObjectAttributes(&Attributes, &Name, 0, nullptr, 0);

        wil::unique_hfile Symlink;
        IO_STATUS_BLOCK IoStatus;
        THROW_IF_NTSTATUS_FAILED(NtCreateFile(
            &Symlink, FILE_GENERIC_READ, &Attributes, &IoStatus, nullptr, 0, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), FILE_OPEN, FILE_OPEN_REPARSE_POINT, NULL, 0));

        FILE_ATTRIBUTE_TAG_INFORMATION Info;
        THROW_IF_NTSTATUS_FAILED(NtQueryInformationFile(Symlink.get(), &IoStatus, &Info, sizeof(Info), FileAttributeTagInformation));

        VERIFY_IS_TRUE((Info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
        if (Directory != false)
        {
            VERIFY_IS_TRUE((Info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
        else
        {
            VERIFY_IS_TRUE((Info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
        }

        VERIFY_ARE_EQUAL(Info.ReparseTag, IO_REPARSE_TAG_SYMLINK);

        union
        {
            char Buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
            REPARSE_DATA_BUFFER Data;
        } ReparseData;

        THROW_IF_NTSTATUS_FAILED(NtFsControlFile(
            Symlink.get(), nullptr, nullptr, nullptr, &IoStatus, FSCTL_GET_REPARSE_POINT, nullptr, 0, &ReparseData, sizeof(ReparseData)));

        VERIFY_ARE_EQUAL(ReparseData.Data.ReparseTag, IO_REPARSE_TAG_SYMLINK);
        VERIFY_ARE_EQUAL(ReparseData.Data.SymbolicLinkReparseBuffer.Flags, (ULONG)SYMLINK_FLAG_RELATIVE);
        const std::wstring Target(
            (PWCHAR)((PCHAR)ReparseData.Data.SymbolicLinkReparseBuffer.PathBuffer + ReparseData.Data.SymbolicLinkReparseBuffer.SubstituteNameOffset),
            (PWCHAR)((PCHAR)ReparseData.Data.SymbolicLinkReparseBuffer.PathBuffer + ReparseData.Data.SymbolicLinkReparseBuffer.SubstituteNameOffset +
                     ReparseData.Data.SymbolicLinkReparseBuffer.SubstituteNameLength));

        VERIFY_ARE_EQUAL(Target, ExpectedTarget);
        const std::wstring Target2(
            (PWCHAR)((PCHAR)ReparseData.Data.SymbolicLinkReparseBuffer.PathBuffer + ReparseData.Data.SymbolicLinkReparseBuffer.SubstituteNameOffset),
            (PWCHAR)((PCHAR)ReparseData.Data.SymbolicLinkReparseBuffer.PathBuffer + ReparseData.Data.SymbolicLinkReparseBuffer.SubstituteNameOffset +
                     ReparseData.Data.SymbolicLinkReparseBuffer.SubstituteNameLength));

        VERIFY_ARE_EQUAL(Target2, ExpectedTarget);
    }

    static VOID VerifyDrvFsLxSymlink(const std::wstring& Path)
    {

        const std::wstring NtPath = L"\\DosDevices\\" + Path;
        UNICODE_STRING Name;
        RtlInitUnicodeString(&Name, NtPath.c_str());
        OBJECT_ATTRIBUTES Attributes;
        InitializeObjectAttributes(&Attributes, &Name, 0, nullptr, 0);

        IO_STATUS_BLOCK IoStatus;
        FILE_STAT_INFORMATION Info;
        THROW_IF_NTSTATUS_FAILED(NtQueryInformationByName(&Attributes, &IoStatus, &Info, sizeof(Info), FileStatInformation));

        VERIFY_IS_TRUE((Info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
        VERIFY_IS_TRUE((Info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
        VERIFY_ARE_EQUAL(Info.ReparseTag, IO_REPARSE_TAG_LX_SYMLINK);
    }
};

class WSL1 : public DrvFsTests
{
    WSL_TEST_CLASS(WSL1)

    bool m_initialized{false};
    TEST_CLASS_SETUP(TestClassSetup)
    {
        if (LxsstuVmMode())
        {
            LogSkipped("This test class is only applicable to WSL1");
        }
        else
        {
            VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(LXSST_TESTS_INSTALL_COMMAND_LINE), 0);
            m_initialized = true;
        }

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (m_initialized)
        {
            LxsstuUninitialize(FALSE);
        }

        return true;
    }

    TEST_METHOD(DrvFsDisableQueryByName)
    {
        WSL1_TEST_ONLY();
        VERIFY_NO_THROW(DrvFsCommon(LX_DRVFS_DISABLE_QUERY_BY_NAME));
    }

    TEST_METHOD(DrvFsDisableQueryByNameAndStatInfo)
    {
        WSL1_TEST_ONLY();
        VERIFY_NO_THROW(DrvFsCommon(LX_DRVFS_DISABLE_QUERY_BY_NAME_AND_STAT_INFO));
    }

    TEST_METHOD(VfsAccessDrvFs)
    {
        WSL1_TEST_ONLY();
        DrvFsTests::VfsAccessDrvFs();
    }

    TEST_METHOD(FsCommonDrvFs)
    {
        WSL1_TEST_ONLY();
        DrvFsTests::FsCommonDrvFs();
    }

    TEST_METHOD(DrvFs)
    {
        WSL1_TEST_ONLY();
        DrvFsTests::DrvFs(DrvFsMode::WSL1);
    }

    TEST_METHOD(DrvFsFat)
    {
        WSL1_TEST_ONLY();
        DrvFsTests::DrvFsFat();
    }

    TEST_METHOD(DrvFsSmb)
    {
        WSL1_TEST_ONLY();
        DrvFsTests::DrvFsSmb();
    }

    TEST_METHOD(DrvFsMetadata)
    {
        WSL1_TEST_ONLY();
        DrvFsTests::DrvFsMetadata(DrvFsMode::WSL1);
    }

    TEST_METHOD(XattrDrvFs)
    {
        WSL1_TEST_ONLY();
        DrvFsTests::XattrDrvFs(DrvFsMode::WSL1);
    }
};

#define WSL2_DRVFS_TEST_CLASS(_mode) \
    class WSL2##_mode## : public DrvFsTests \
    { \
        WSL_TEST_CLASS(WSL2##_mode##) \
        std::unique_ptr<WslConfigChange> m_config; \
        TEST_CLASS_SETUP(TestClassSetup) \
        { \
            if (!LxsstuVmMode()) \
            { \
                LogSkipped("This test class is only applicable to WSL2"); \
            } \
            else \
            { \
                VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE); \
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(LXSST_TESTS_INSTALL_COMMAND_LINE), 0); \
                m_config.reset(new WslConfigChange(LxssGenerateTestConfig({.drvFsMode = DrvFsMode::##_mode##}))); \
            } \
\
            return true; \
        } \
\
        TEST_CLASS_CLEANUP(TestClassCleanup) \
        { \
            if (m_config) \
            { \
                m_config.reset(); \
                LxsstuUninitialize(FALSE); \
            } \
\
            return true; \
        } \
\
        TEST_METHOD(VfsAccessDrvFs) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::VfsAccessDrvFs(); \
        } \
\
        TEST_METHOD(FsCommonDrvFs) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::FsCommonDrvFs(); \
        } \
\
        TEST_METHOD(DrvFs) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvFs(DrvFsMode::##_mode##); \
        } \
\
        TEST_METHOD(DrvFsFat) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvFsFat(); \
        } \
\
        TEST_METHOD(DrvFsSmb) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvFsSmb(); \
        } \
\
        TEST_METHOD(DrvFsMetadata) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvFsMetadata(DrvFsMode::##_mode##); \
        } \
\
        TEST_METHOD(DrvfsMountElevated) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvfsMountElevated(DrvFsMode::##_mode##); \
        } \
\
        TEST_METHOD(DrvfsMountElevatedDifferentConsole) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvfsMountElevatedDifferentConsole(DrvFsMode::##_mode##); \
        } \
\
        TEST_METHOD(DrvfsMountNonElevated) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvfsMountNonElevated(DrvFsMode::##_mode##); \
        } \
\
        TEST_METHOD(DrvfsMountNonElevatedDifferentConsole) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvfsMountNonElevatedDifferentConsole(DrvFsMode::##_mode##); \
        } \
\
        TEST_METHOD(XattrDrvFs) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::XattrDrvFs(DrvFsMode::##_mode##); \
        } \
\
        TEST_METHOD(DrvFsReFs) \
        { \
            WSL2_TEST_ONLY(); \
            DrvFsTests::DrvFsReFs(); \
        } \
    }

WSL2_DRVFS_TEST_CLASS(Plan9);

// Disabled while an issue with the 6.1 Linux kernel causing disk corruption is investigated.
// TODO: Enable again once the issue is resolved
// WSL2_DRVFS_TEST_CLASS(Virtio9p);

// Disabled because it causes too much noise.
// TODO: Enable again once virtiofs is stable
// WSL2_DRVFS_TEST_CLASS(VirtioFs);

} // namespace DrvFsTests