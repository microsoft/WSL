/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    vfsaccess.c

Abstract:

    This file is a vfs access permissions test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <utime.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <limits.h>
#include "lxtfs.h"

#if !defined(__amd64__) && !defined(__aarch64__)

#include <sys/capability.h>

#else

#include <sys/cdefs.h>
#include <linux/capability.h>

#define _LINUX_CAPABILITY_VERSION_3 0x20080522

#ifndef O_PATH
#define O_PATH 010000000
#endif

#endif

#include <sys/vfs.h>
#include <linux/prctl.h>
#include <stdio.h>

#define LXT_NAME "vfsaccess"
#define LXT_NAME_DRVFS "vfsaccess_drvfs"

#define VFS_FILE_OBJECT_COUNT LXT_COUNT_OF(g_VfsFileObjectFlags)

#define VFS_FILE_CONTENTS "vfsaccesstestfilecontents"

#define VFS_ACCESS_UID 1012

#define VFS_ACCESS_PARENT_DIR "/data/test/vfsaccesstest"

#define VFS_ACCESS_CHMOD_DIR VFS_ACCESS_PARENT_DIR "/vfsaccessdir_chmod"

#define VFS_ACCESS_OPATH_DIR VFS_ACCESS_PARENT_DIR "/vfsaccessopathdir"

#define VFS_ACCESS_OPATH_FILE VFS_ACCESS_PARENT_DIR "/vfsaccessopath"

#define VFS_ACCESS_OPATH_FILE_LINK VFS_ACCESS_PARENT_DIR "/vfsaccessopathlink"

#define VFS_ACCESS_STICKY_BIT_DIR VFS_ACCESS_PARENT_DIR "/vfsaccessdir_stickybit"

#define VFS_ACCESS_GROUP_USER_ID_DIR VFS_ACCESS_PARENT_DIR "/vfsaccessdir_groupuserid"

#define VFS_ACCESS_UTIME_FILE VFS_ACCESS_PARENT_DIR "/vfsaccesutime"

#define VFS_ACCESS_FSUID_FILE VFS_ACCESS_PARENT_DIR "/setfsuid_testfile"

#define VFS_ACCESS_FIFO VFS_ACCESS_PARENT_DIR "/vfsaccess_fifo"

#define O_NOACCESS (O_WRONLY | O_RDWR)

#define VFS_ACCESS_EXECVE_TEST_RESULT (123)

typedef struct _VFS_ACCESS_FILE
{
    char* Name;
    mode_t Mode;
} VFS_ACCESS_FILE, *PVFS_ACCESS_FILE;

typedef struct _VFS_ACCESS_FILE_OBJECT
{
    int Fd;
    int Flags;
} VFS_ACCESS_FILE_OBJECT, *PVFS_ACCESS_FILE_OBJECT;

struct reuid_t
{
    uid_t r;
    uid_t e;
    uid_t s;
};

int VfsAccessCheckResult(int ResultActual, int ResultExpected, int ErrnoActual, int ErrnoExpected, char* Message, int VariationIndex);

void VfsAccessFileObjectCleanup(void);

int VfsAccessFileObjectCreateFiles(void);

int VfsAccessFileObjectCreateSymlinks(void);

int VfsAccessFileObjectOpenFiles(VFS_ACCESS_FILE_OBJECT Files[]);

int VfsAccessFileObjectOpenSymlinks(VFS_ACCESS_FILE_OBJECT Files[]);

int VfsAccessFileObjectChecks(PLXT_ARGS Args);

int VfsAccessFileObjectSymlinksChecks(PLXT_ARGS Args);

int VfsAccessRemapReference(PLXT_ARGS Args);

int VfsAccessChmod(PLXT_ARGS Args);

int VfsAccessChmodCap(PLXT_ARGS Args);

LXT_VARIATION_HANDLER VfsAccessFifo;

int VfsAccessOPath(PLXT_ARGS Args);

int VfsAccessStickyBit(PLXT_ARGS Args);

int VfsAccessSetUserGroupId(PLXT_ARGS Args);

int VfsAccessSetUserGroupIdExecveChild(void);

int VfsAccessInodeChecks(PLXT_ARGS Args);

int VfsAccessParseArgs(int Argc, char* Argv[], LXT_ARGS* Args);

int VfsAccessUTimeCap(PLXT_ARGS Args);

int VfsAccessSetFsUid(PLXT_ARGS Args);

int VfsAccessSetUid(PLXT_ARGS Args);

//
// Global constants
//

#define VFS_ACCESS_FILE_OBJECT_FILE 0
#define VFS_ACCESS_REMAP_FILE 1

static const VFS_ACCESS_FILE g_VfsFiles[] = {
    {VFS_ACCESS_PARENT_DIR "/vfsaccessfile", S_IRWXU | S_IRWXG | S_IRWXO},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessfile_remap", S_IRWXU | S_IRWXG | S_IRWXO}};

static const char* g_VfsSymlinks[] = {
    VFS_ACCESS_PARENT_DIR "/sym_vfsaccessfile", VFS_ACCESS_PARENT_DIR "/sym_vfsaccessfile_remap"};

static const int g_VfsFileObjectFlags[] = {O_RDONLY, O_WRONLY, O_RDWR, O_NOACCESS, O_RDONLY | O_PATH, O_RDWR | O_APPEND};

static const VFS_ACCESS_FILE g_VfsInodeEntries[] = {
    {VFS_ACCESS_PARENT_DIR "/vfsaccessfile_r", S_IFREG | S_IRUSR | S_IRGRP | S_IROTH},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessfile_w", S_IFREG | S_IWUSR | S_IWGRP | S_IWOTH},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessfile_x", S_IFREG | S_IXUSR | S_IXGRP | S_IXOTH},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessfile_rw", S_IFREG | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessdir_r", S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessdir_w", S_IFDIR | S_IWUSR | S_IWGRP | S_IWOTH},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessdir_x", S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH},
    {VFS_ACCESS_PARENT_DIR "/vfsaccessdir_wx", S_IFDIR | S_IWUSR | S_IWGRP | S_IWOTH | S_IXUSR | S_IXGRP | S_IXOTH}};

#define VFS_ACCESS_INODE_ENTRY_FILE "vfsaccessfile"

int g_VfsSetFsUidCaps[] = {
    CAP_CHOWN, CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH, CAP_FOWNER, CAP_FSETID, CAP_LINUX_IMMUTABLE, CAP_MAC_OVERRIDE, CAP_MKNOD};

static const LXT_VARIATION g_LxtVariations[] = {
    {"VfsAccess file object checks", VfsAccessFileObjectChecks},
    {"VfsAccess symlinks checks", VfsAccessFileObjectSymlinksChecks},
    {"VfsAccess remap reference", VfsAccessRemapReference},
    {"VfsAccess chmod", VfsAccessChmod},
    {"VfsAccess chmod cap", VfsAccessChmodCap},
    {"VfsAccess O_PATH", VfsAccessOPath},
    {"VfsAccess sticky bit", VfsAccessStickyBit},
    {"VfsAccess set-user-ID set-group-ID", VfsAccessSetUserGroupId},
    {"VfsAccess inode checks", VfsAccessInodeChecks},
    {"VfsAccess utime cap", VfsAccessUTimeCap},
    {"VfsAccess setfsuid", VfsAccessSetFsUid},
    //{"VfsAccess Fifo", VfsAccessFifo},
    {"VfsAccess set*uid", VfsAccessSetUid}};

bool g_UseDrvFs = false;

int VfsAccessTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    if ((Argc == 2) && (strcmp(Argv[1], "execvetest") == 0))
    {
        return VFS_ACCESS_EXECVE_TEST_RESULT;
    }

    LxtCheckResult(VfsAccessParseArgs(Argc, Argv, &Args));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

void VfsAccessFileObjectCleanup(void)

/*++

Description:

    This routine cleans up test files.

Arguments:

    None.

Return Value:

    None.

--*/

{

    int Index;

    for (Index = 0; Index < LXT_COUNT_OF(g_VfsSymlinks); Index += 1)
    {
        unlink(g_VfsSymlinks[Index]);
    }

    for (Index = 0; Index < LXT_COUNT_OF(g_VfsFiles); ++Index)
    {
        unlink(g_VfsFiles[Index].Name);
    }

    for (Index = 0; Index < LXT_COUNT_OF(g_VfsInodeEntries); ++Index)
    {
        if (S_ISREG(g_VfsInodeEntries[Index].Mode))
        {
            unlink(g_VfsInodeEntries[Index].Name);
        }
        else
        {
            rmdir(g_VfsInodeEntries[Index].Name);
        }
    }

    return;
}

int VfsAccessFileObjectCreateFiles(void)

/*++
--*/

{

    unsigned int BytesWritten;
    unsigned int Index;
    int Fd;
    int Result;

    Fd = -1;
    for (Index = 0; Index < LXT_COUNT_OF(g_VfsFiles); ++Index)
    {
        unlink(g_VfsFiles[Index].Name);
        LxtCheckErrno(Fd = open(g_VfsFiles[Index].Name, O_RDWR | O_CREAT, g_VfsFiles[Index].Mode));
        LxtCheckErrno(write(Fd, VFS_FILE_CONTENTS, sizeof(VFS_FILE_CONTENTS)));
        if (Index == VFS_ACCESS_REMAP_FILE)
        {
            BytesWritten = sizeof(VFS_FILE_CONTENTS);
            while (BytesWritten < 2 * PAGE_SIZE)
            {
                LxtCheckErrno(write(Fd, VFS_FILE_CONTENTS, sizeof(VFS_FILE_CONTENTS)));
                BytesWritten += sizeof(VFS_FILE_CONTENTS);
            }
        }

        LxtClose(Fd);
        Fd = -1;
    }

    for (Index = 0; Index < LXT_COUNT_OF(g_VfsInodeEntries); ++Index)
    {
        if (S_ISREG(g_VfsInodeEntries[Index].Mode))
        {
            unlink(g_VfsInodeEntries[Index].Name);
            LxtCheckErrno(Fd = open(g_VfsInodeEntries[Index].Name, O_RDWR | O_CREAT, g_VfsInodeEntries[Index].Mode));

            LxtCheckErrno(write(Fd, VFS_FILE_CONTENTS, sizeof(VFS_FILE_CONTENTS)));
            LxtClose(Fd);
            Fd = -1;
        }
        else
        {
            rmdir(g_VfsInodeEntries[Index].Name);
            LxtCheckErrno(mkdir(g_VfsInodeEntries[Index].Name, g_VfsInodeEntries[Index].Mode));
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int VfsAccessFileObjectCreateSymlinks(void)

/*++
--*/

{

    unsigned int Index;
    int IntermediateResult;
    int Result;

    LxtCheckEqual(LXT_COUNT_OF(g_VfsFiles), LXT_COUNT_OF(g_VfsSymlinks), "%d");

    //
    // Create symlinks for the files.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_VfsFiles); Index += 1)
    {
        IntermediateResult = symlink(g_VfsFiles[Index].Name, g_VfsSymlinks[Index]);

        //
        // The symlink call may fail if the symlink already exists. This is ok
        // in order to run the unit test on the same machine multiple times.
        //

        if (IntermediateResult < 0)
        {
            LxtCheckErrnoFailure(IntermediateResult, EEXIST);
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessCheckResult(int ResultActual, int ResultExpected, int ErrnoActual, int ErrnoExpected, char* Message, int VariationIndex)

/*++
--*/

{

    int Result;

    Result = LXT_RESULT_FAILURE;
    if (ResultActual != ResultExpected)
    {
        if (ResultActual >= 0)
        {
            ErrnoActual = 0;
        }

        LxtLogError("Unexpected %s (%d) result actual %d (%s) != expected %d", Message, VariationIndex, ResultActual, strerror(ErrnoActual), ResultExpected);

        goto ErrorExit;
    }

    if ((ResultActual == -1) && (ErrnoActual != ErrnoExpected))
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected %s (%d) errno actual %s != expected %s", Message, VariationIndex, strerror(ErrnoActual), strerror(ErrnoExpected));

        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessFifo(PLXT_ARGS Args)

/*++

Description:

    This routine tests access permissions on fifos.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildPid;
    int Result;

    LxtCheckErrnoZeroSuccess(mkfifo(VFS_ACCESS_FIFO, 0600));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        setgid(1000);
        setuid(1000);
        LxtCheckErrnoFailure(open(VFS_ACCESS_FIFO, O_RDONLY | O_NONBLOCK), EACCES);

        LxtCheckErrnoFailure(open(VFS_ACCESS_FIFO, O_WRONLY | O_NONBLOCK), EACCES);

        exit(0);
    }

    LxtWaitPidPoll(ChildPid, 0);

ErrorExit:
    unlink(VFS_ACCESS_FIFO);
    return Result;
}

int VfsAccessFileObjectChecks(PLXT_ARGS Args)

/*++
--*/

{

    int AccessMode;
    char Buffer;
    int ErrnoExpected;
    VFS_ACCESS_FILE_OBJECT Files[VFS_FILE_OBJECT_COUNT];
    unsigned int Index;
    void* Map;
    int Result;
    int ResultActual;
    int ResultExpected;
    bool VirtiofsNoDax;

    LxtLogInfo("Fs type %d with dax = %d\n", g_LxtFsInfo.FsType, g_LxtFsInfo.Flags.Dax);
    VirtiofsNoDax = g_LxtFsInfo.FsType == LxtFsTypeVirtioFs && g_LxtFsInfo.Flags.Dax == 0;
    memset(Files, -1, sizeof(Files));
    LxtCheckResult(VfsAccessFileObjectOpenFiles(Files));
    for (Index = 0; Index < VFS_FILE_OBJECT_COUNT; ++Index)
    {

        //
        // Validate read with a valid buffer.
        //

        ResultExpected = -1;
        if ((Files[Index].Flags == O_RDONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 1;
        }

        ResultActual = LxtRead(Files[Index].Fd, &Buffer, 1);
        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EBADF, "read", Index));

        //
        // Validate read with a invalid buffer and a size of 0. The size of 0
        // should cause the buffer to not be checked.
        //

        ResultExpected = -1;
        if ((Files[Index].Flags == O_RDONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 0;
        }

        ResultActual = LxtRead(Files[Index].Fd, (void*)0x1, 0);
        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EBADF, "read", Index));

        //
        // Validate write with a valid buffer.
        //

        ResultExpected = -1;
        if ((Files[Index].Flags == O_WRONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 1;
        }

        ResultActual = LxtWrite(Files[Index].Fd, &Buffer, 1);
        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EBADF, "write", Index));

        //
        // Validate write with a invalid buffer and a size of 0. The size of 0
        // should cause the buffer to not be checked.
        //

        ResultExpected = -1;
        if ((Files[Index].Flags == O_WRONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 0;
        }

        ResultActual = LxtWrite(Files[Index].Fd, (void*)0x1, 0);
        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EBADF, "write", Index));

        //
        // Validate map read shared and upgrading to write access.
        //
        // N.B. The Linux 9p client does not allow mapping shared if the file is
        //      opened for write.
        //
        // N.B. The virtiofs device relies on fuse mapping which only supports
        //      shared in the presence of DAX.
        //

        ErrnoExpected = EACCES;
        if (Files[Index].Flags == O_PATH)
        {
            ErrnoExpected = EBADF;
        }
        else if (VirtiofsNoDax && (Files[Index].Flags == O_RDONLY || (Files[Index].Flags & O_ACCMODE) == O_RDWR))
        {
            ErrnoExpected = ENODEV;
        }

        ResultExpected = -1;
        if (!VirtiofsNoDax && (((Files[Index].Flags & O_ACCMODE) == O_RDONLY) || ((Files[Index].Flags & O_ACCMODE) == O_RDWR)) &&
            ((Files[Index].Flags & O_PATH) == 0))
        {

            ResultExpected = 1;
        }

        Map = mmap(NULL, sizeof(Buffer), PROT_READ, MAP_SHARED, Files[Index].Fd, 0);
        ResultActual = -1;
        if (Map != MAP_FAILED)
        {
            ResultActual = 1;
        }

        LxtLogInfo("%d, %d, %d", Index, Files[Index].Flags, ResultExpected);
        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap read shared", Index));

        if (Map != MAP_FAILED)
        {
            ResultExpected = -1;
            if ((Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
            {

                ResultExpected = 0;
            }

            ResultActual = mprotect(Map, sizeof(Buffer), PROT_WRITE);
            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap read shared mprotect", Index));

            LxtCheckErrno(LxtMunmap(Map, sizeof(Buffer)));
        }

        //
        // Validate map read private and upgrading to write access.
        //
        // N.B. The Linux 9p client does not allow mapping shared if the file is
        //      opened for write.
        //

        ResultExpected = -1;
        if ((Files[Index].Flags == O_RDONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 1;
        }

        Map = mmap(NULL, sizeof(Buffer), PROT_READ, MAP_PRIVATE, Files[Index].Fd, 0);
        ResultActual = -1;
        if (Map != MAP_FAILED)
        {
            ResultActual = 1;
        }

        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap read private", Index));

        if (Map != MAP_FAILED)
        {
            ResultExpected = -1;
            if ((Files[Index].Flags == O_RDONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
            {

                ResultExpected = 0;
            }

            ResultActual = mprotect(Map, sizeof(Buffer), PROT_WRITE);
            VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap read private mprotect", Index);

            LxtCheckErrno(LxtMunmap(Map, sizeof(Buffer)));
        }

        //
        // Validate map write shared and private
        //
        // N.B. The virtiofs device relies on fuse mapping which only supports
        //      shared in the presence of DAX.
        //

        ErrnoExpected = EACCES;
        if (Files[Index].Flags == O_PATH)
        {
            ErrnoExpected = EBADF;
        }
        else if (VirtiofsNoDax && (Files[Index].Flags & O_ACCMODE) == O_RDWR)
        {
            ErrnoExpected = ENODEV;
        }

        ResultExpected = -1;
        if (!VirtiofsNoDax && (Files[Index].Flags & O_ACCMODE) == O_RDWR)
        {
            ResultExpected = 1;
        }

        Map = mmap(NULL, sizeof(Buffer), PROT_WRITE, MAP_SHARED, Files[Index].Fd, 0);
        ResultActual = -1;
        if (Map != MAP_FAILED)
        {
            ResultActual = 1;
            LxtCheckErrno(LxtMunmap(Map, sizeof(Buffer)));
        }

        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap write shared", Index));

        ResultExpected = -1;
        if ((Files[Index].Flags == O_RDONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 1;
        }

        Map = mmap(NULL, sizeof(Buffer), PROT_WRITE, MAP_PRIVATE, Files[Index].Fd, 0);
        ResultActual = -1;
        if (Map != MAP_FAILED)
        {
            ResultActual = 1;
            LxtCheckErrno(LxtMunmap(Map, sizeof(Buffer)));
        }

        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap write private", Index));

        LxtClose(Files[Index].Fd);
        Files[Index].Fd = -1;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessFileObjectSymlinksChecks(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer;
    int ErrnoExpected;
    VFS_ACCESS_FILE_OBJECT Files[VFS_FILE_OBJECT_COUNT];
    unsigned int Index;
    void* Map;
    int Result;
    int ResultActual;
    int ResultExpected;
    bool VirtiofsNoDax;

    LxtLogInfo("Fs type %d with dax = %d\n", g_LxtFsInfo.FsType, g_LxtFsInfo.Flags.Dax);
    VirtiofsNoDax = g_LxtFsInfo.FsType == LxtFsTypeVirtioFs && g_LxtFsInfo.Flags.Dax == 0;

    memset(Files, -1, sizeof(Files));
    LxtCheckResult(VfsAccessFileObjectOpenSymlinks(Files));
    for (Index = 0; Index < VFS_FILE_OBJECT_COUNT; ++Index)
    {

        //
        // Validate read
        //

        ResultExpected = -1;
        if ((Files[Index].Flags == O_RDONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 1;
        }

        ResultActual = LxtRead(Files[Index].Fd, &Buffer, 1);
        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EBADF, "read", Index));

        //
        // Validate write
        //

        ResultExpected = -1;
        if ((Files[Index].Flags == O_WRONLY) || (Files[Index].Flags == O_RDWR) || (Files[Index].Flags == (O_RDWR | O_APPEND)))
        {

            ResultExpected = 1;
        }

        ResultActual = LxtWrite(Files[Index].Fd, &Buffer, 1);
        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EBADF, "write", Index));

        //
        // Validate map read
        //

        ErrnoExpected = EACCES;
        if (Files[Index].Flags == O_PATH)
        {
            ErrnoExpected = EBADF;
        }
        else if (VirtiofsNoDax && (Files[Index].Flags == O_RDONLY || (Files[Index].Flags & O_ACCMODE) == O_RDWR))
        {
            ErrnoExpected = ENODEV;
        }

        ResultExpected = -1;
        if (!VirtiofsNoDax && (((Files[Index].Flags & O_ACCMODE) == O_RDONLY) || ((Files[Index].Flags & O_ACCMODE) == O_RDWR)) &&
            ((Files[Index].Flags & O_PATH) == 0))
        {

            ResultExpected = 1;
        }

        Map = mmap(NULL, sizeof(Buffer), PROT_READ, MAP_SHARED, Files[Index].Fd, 0);
        ResultActual = -1;
        if (Map != MAP_FAILED)
        {
            ResultActual = 1;
            LxtCheckErrno(LxtMunmap(Map, sizeof(Buffer)));
        }

        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap read", Index));

        //
        // Validate map write
        //

        ErrnoExpected = EACCES;
        if (Files[Index].Flags == O_PATH)
        {
            ErrnoExpected = EBADF;
        }
        else if (VirtiofsNoDax && (Files[Index].Flags & O_ACCMODE) == O_RDWR)
        {
            ErrnoExpected = ENODEV;
        }

        ResultExpected = -1;
        if (!VirtiofsNoDax && (Files[Index].Flags & O_ACCMODE) == O_RDWR)
        {
            ResultExpected = 1;
        }

        Map = mmap(NULL, sizeof(Buffer), PROT_WRITE, MAP_SHARED, Files[Index].Fd, 0);
        ResultActual = -1;
        if (Map != MAP_FAILED)
        {
            ResultActual = 1;
            LxtCheckErrno(LxtMunmap(Map, sizeof(Buffer)));
        }

        LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "mmap write", Index));

        LxtClose(Files[Index].Fd);
        Files[Index].Fd = -1;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessFileObjectOpenFiles(VFS_ACCESS_FILE_OBJECT Files[])

/*++
--*/

{

    unsigned int Index;
    int Result;

    for (Index = 0; Index < VFS_FILE_OBJECT_COUNT; ++Index)
    {
        Files[Index].Flags = g_VfsFileObjectFlags[Index];
        LxtCheckErrno(Files[Index].Fd = open(g_VfsFiles[VFS_ACCESS_FILE_OBJECT_FILE].Name, Files[Index].Flags, 0));
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessFileObjectOpenSymlinks(VFS_ACCESS_FILE_OBJECT Files[])

/*++
--*/

{

    unsigned int Index;
    int Result;

    for (Index = 0; Index < VFS_FILE_OBJECT_COUNT; Index += 1)
    {
        Files[Index].Flags = g_VfsFileObjectFlags[Index];
        LxtCheckErrno(Files[Index].Fd = open(g_VfsSymlinks[VFS_ACCESS_FILE_OBJECT_FILE], Files[Index].Flags, 0));
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessRemapReference(PLXT_ARGS Args)

{

    char Buffer;
    int FdReadOnly = -1;
    int FdReadWrite = -1;
    int MapFlags;
    void* MapResult;
    void* MapReadOnly = NULL;
    void* MapReadWrite = NULL;
    void* RemappedMemory = NULL;
    int Result;
    bool VirtiofsNoDax;

    VirtiofsNoDax = g_LxtFsInfo.FsType == LxtFsTypeVirtioFs && g_LxtFsInfo.Flags.Dax == 0;
    MapFlags = VirtiofsNoDax ? MAP_PRIVATE : MAP_SHARED;

    //
    // Open and map a file whose only reference is read only and open second
    // file descriptor and mapping read write.
    //

    LxtCheckErrno(FdReadOnly = open(g_VfsFiles[VFS_ACCESS_REMAP_FILE].Name, O_RDONLY, 0));
    LxtCheckMapErrno(MapReadOnly = mmap(NULL, sizeof(Buffer), PROT_READ, MapFlags, FdReadOnly, 0));
    LxtCheckErrno(FdReadWrite = open(g_VfsFiles[VFS_ACCESS_REMAP_FILE].Name, O_RDWR, 0));
    LxtCheckMapErrno(MapReadWrite = mmap(NULL, sizeof(Buffer), PROT_READ | PROT_WRITE, MapFlags, FdReadWrite, 0));
    LxtCheckMapErrno(RemappedMemory = mremap(MapReadWrite, sizeof(Buffer), PAGE_SIZE * 2, MREMAP_MAYMOVE));

ErrorExit:
    if (FdReadOnly != -1)
    {
        if (MapReadOnly != NULL)
        {
            LxtMunmap(MapReadOnly, sizeof(Buffer));
        }

        LxtClose(FdReadOnly);
    }

    if (FdReadWrite != -1)
    {
        if (RemappedMemory != NULL)
        {
            LxtMunmap(RemappedMemory, PAGE_SIZE * 2);
        }
        else if (MapReadWrite != NULL)
        {
            LxtMunmap(MapReadWrite, sizeof(Buffer));
        }

        LxtClose(FdReadWrite);
    }

    return Result;
}

int VfsAccessChmod(PLXT_ARGS Args)

/*++
--*/

{

    int DirFd;
    int Result;
    struct stat StatBuf;

    DirFd = -1;

    //
    // Set bits with chmod and then fchmod.
    //

    rmdir(VFS_ACCESS_CHMOD_DIR);
    LxtCheckErrno(mkdir(VFS_ACCESS_CHMOD_DIR, S_IRWXU));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_IRWXU | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != S_IRWXU | S_IFDIR", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(chmod(VFS_ACCESS_CHMOD_DIR, S_IRWXG));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_IRWXG | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != S_IRWXG | S_IFDIR", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(chmod(VFS_ACCESS_CHMOD_DIR, S_IRWXO));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_IRWXO | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != S_IRWXO | S_IFDIR", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(chmod(VFS_ACCESS_CHMOD_DIR, S_IRWXU));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_IRWXU | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != S_IRWXU | S_IFDIR", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(chmod(VFS_ACCESS_CHMOD_DIR, 0xffff));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_ISVTX | S_ISGID | S_ISUID | S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != All bits", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(DirFd = open(VFS_ACCESS_CHMOD_DIR, O_DIRECTORY | O_RDONLY, 0));
    LxtCheckErrno(fchmod(DirFd, S_IRWXG));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_IRWXG | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != S_IRWXG | S_IFDIR", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(fchmod(DirFd, S_IRWXO));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_IRWXO | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != S_IRWXO | S_IFDIR", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(fchmod(DirFd, S_IRWXU));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_IRWXU | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != S_IRWXU | S_IFDIR", StatBuf.st_mode);
        goto ErrorExit;
    }

    LxtCheckErrno(fchmod(DirFd, 0xffff));
    LxtCheckErrno(stat(VFS_ACCESS_CHMOD_DIR, &StatBuf));
    if (StatBuf.st_mode != (S_ISVTX | S_ISGID | S_ISUID | S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR))
    {
        LxtLogError("Unexpected mode %d != All bits", StatBuf.st_mode);
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (DirFd != -1)
    {
        LxtClose(DirFd);
    }

    rmdir(VFS_ACCESS_CHMOD_DIR);
    return Result;
}

void VfsAccessChmodCapChild(void)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int Result;

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
    CapData[CAP_TO_INDEX(CAP_DAC_OVERRIDE)].permitted |= CAP_TO_MASK(CAP_DAC_OVERRIDE);
    CapData[CAP_TO_INDEX(CAP_CHOWN)].permitted |= CAP_TO_MASK(CAP_CHOWN);
    CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
    CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
    CapData[0].effective = CapData[0].permitted;
    CapData[1].effective = CapData[1].permitted;

    //
    // Drop privileges so the current process does not have CAP_FOWNER.
    //

    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(setgid(VFS_ACCESS_UID));
    LxtCheckErrno(setuid(VFS_ACCESS_UID));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    //
    // TODO: Update the below when checks are enforced
    //

    //
    // Try to chmod the directory to the current value.
    //

    LxtCheckErrnoFailure(chmod(VFS_ACCESS_CHMOD_DIR, S_IRWXU), EPERM);

    //
    // Try to chmod on the directory without CAP_FOWNER to 0751.
    //

    LxtCheckErrnoFailure(chmod(VFS_ACCESS_CHMOD_DIR, S_IRWXU | S_IRGRP | S_IXGRP | S_IXOTH), EPERM);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    _exit(Result);
}

int VfsAccessChmodCap(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int Result;

    rmdir(VFS_ACCESS_CHMOD_DIR);
    LxtCheckErrno(mkdir(VFS_ACCESS_CHMOD_DIR, S_IRWXU));
    ChildPid = fork();
    if (ChildPid == 0)
    {
        VfsAccessChmodCapChild();
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    rmdir(VFS_ACCESS_CHMOD_DIR);
    return Result;
}

void VfsAccessOPathChild(void)

/*++
--*/

{

    char Buffer[100];
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int Fd;
    int Result;
    struct stat StatBuffer;
    struct statfs StatFsBuffer;
    struct timespec Times[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
    CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
    CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
    CapData[0].effective = CapData[0].permitted;
    CapData[1].effective = CapData[1].permitted;
    Fd = -1;

    //
    // Drop privileges so the current process does not have VFS related
    // capabilities.
    //

    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    //
    // Open the file with O_PATH and check the behavior for the syscalls of
    // interest.
    //

    LxtCheckErrno(Fd = open(VFS_ACCESS_OPATH_FILE, O_PATH, 0));

    //
    // Check syscalls that take a file descriptor should fail because O_PATH was
    // specified.
    //

    LxtCheckErrnoFailure(fchmod(Fd, 0), EBADF);
    LxtCheckErrnoFailure(fchown(Fd, 0, 0), EBADF);
    LxtCheckErrnoFailure(fsync(Fd), EBADF);
    LxtCheckErrnoFailure(LxtGetdents64(Fd, (struct dirent*)Buffer, sizeof(Buffer)), EBADF);
    LxtCheckErrnoFailure(futimens(Fd, Times), EBADF);
    LxtCheckErrnoFailure(flistxattr(Fd, Buffer, sizeof(Buffer)), EBADF);

    //
    // Check syscalls that take a file descriptor should succeed even through
    // O_PATH was specified.
    //

    LxtCheckErrno(fstat(Fd, &StatBuffer));
    LxtCheckErrno(fstatfs(Fd, &StatFsBuffer));

    //
    // Check syscalls that should succeed on a directory with O_PATH specified.
    //

    LxtCheckErrnoZeroSuccess(close(Fd));
    LxtCheckErrno(Fd = open(VFS_ACCESS_OPATH_DIR, O_PATH | O_DIRECTORY));
    LxtCheckErrnoZeroSuccess(fchdir(Fd));

    //
    // Chdir should still fail if execute permissions are removed.
    //

    LxtCheckErrnoZeroSuccess(chmod(VFS_ACCESS_OPATH_DIR, 0));
    LxtCheckErrnoFailure(fchdir(Fd), EACCES);

    //
    // Check syscalls that take a path should succeed because they do not
    // require access to the file, but instead just the path.
    //

    LxtCheckErrno(chmod(VFS_ACCESS_OPATH_FILE, 0));
    LxtCheckErrno(chown(VFS_ACCESS_OPATH_FILE, 0, 0));
    LxtCheckErrno(stat(VFS_ACCESS_OPATH_FILE, &StatBuffer));
    LxtCheckErrno(statfs(VFS_ACCESS_OPATH_FILE, &StatFsBuffer));
    LxtCheckErrno(readlink(VFS_ACCESS_OPATH_FILE_LINK, Buffer, sizeof(Buffer)));

    //
    // Xattr is not supported on drvfs currently.
    //

    if (g_UseDrvFs == false)
    {
        LxtCheckErrno(listxattr(VFS_ACCESS_OPATH_FILE_LINK, Buffer, sizeof(Buffer)));
        LxtCheckErrno(llistxattr(VFS_ACCESS_OPATH_FILE_LINK, Buffer, sizeof(Buffer)));
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    _exit(Result);
}

int VfsAccessOPath(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int Fd;
    int Result;

    unlink(VFS_ACCESS_OPATH_FILE);
    unlink(VFS_ACCESS_OPATH_FILE_LINK);
    LxtCheckErrno(mkdir(VFS_ACCESS_OPATH_DIR, 0111));
    LxtCheckErrno(Fd = open(VFS_ACCESS_OPATH_FILE, O_CREAT, 0));
    LxtCheckErrno(symlink(VFS_ACCESS_OPATH_FILE, VFS_ACCESS_OPATH_FILE_LINK));
    ChildPid = fork();
    if (ChildPid == 0)
    {
        VfsAccessOPathChild();
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    unlink(VFS_ACCESS_OPATH_FILE);
    unlink(VFS_ACCESS_OPATH_FILE_LINK);
    rmdir(VFS_ACCESS_OPATH_DIR);
    return Result;
}

void VfsAccessRenameCapChild(void)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int File;
    int Result;

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
    CapData[CAP_TO_INDEX(CAP_DAC_OVERRIDE)].permitted |= CAP_TO_MASK(CAP_DAC_OVERRIDE);
    CapData[CAP_TO_INDEX(CAP_CHOWN)].permitted |= CAP_TO_MASK(CAP_CHOWN);
    CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
    CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
    CapData[0].effective = CapData[0].permitted;
    CapData[1].effective = CapData[1].permitted;

    //
    // Drop privileges so the current process does not have CAP_FOWNER.
    //

    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(setgid(VFS_ACCESS_UID));
    LxtCheckErrno(setuid(VFS_ACCESS_UID));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    //
    // Create a file and directory for the current user.
    //

    LxtCheckErrno(mkdir(VFS_ACCESS_STICKY_BIT_DIR "/userdir1", S_IRWXU));
    LxtCheckErrno(File = creat(VFS_ACCESS_STICKY_BIT_DIR "/userfile1", S_IRWXU));
    close(File);

    //
    // Try to rename the file and directory to an existing entry without
    // CAP_FOWNER.
    //

    LxtCheckErrnoFailure(rename(VFS_ACCESS_STICKY_BIT_DIR "/userfile1", VFS_ACCESS_STICKY_BIT_DIR "/file1"), EPERM);

    LxtCheckErrnoFailure(rename(VFS_ACCESS_STICKY_BIT_DIR "/userdir1", VFS_ACCESS_STICKY_BIT_DIR "/dir1"), EPERM);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    rmdir(VFS_ACCESS_STICKY_BIT_DIR "/userdir1");
    remove(VFS_ACCESS_STICKY_BIT_DIR "/userfile1");
    _exit(Result);
}

void VfsAccessRmdirCapChild(void)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int Result;

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
    CapData[CAP_TO_INDEX(CAP_DAC_OVERRIDE)].permitted |= CAP_TO_MASK(CAP_DAC_OVERRIDE);
    CapData[CAP_TO_INDEX(CAP_CHOWN)].permitted |= CAP_TO_MASK(CAP_CHOWN);
    CapData[CAP_TO_INDEX(CAP_SETUID)].permitted |= CAP_TO_MASK(CAP_SETUID);
    CapData[CAP_TO_INDEX(CAP_SETGID)].permitted |= CAP_TO_MASK(CAP_SETGID);
    CapData[0].effective = CapData[0].permitted;
    CapData[1].effective = CapData[1].permitted;

    //
    // Drop privileges so the current process does not have CAP_FOWNER.
    //

    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(setgid(VFS_ACCESS_UID));
    LxtCheckErrno(setuid(VFS_ACCESS_UID));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    //
    // Try to remove the file and directory without CAP_FOWNER.
    //

    LxtCheckErrnoFailure(remove(VFS_ACCESS_STICKY_BIT_DIR "/file1"), EPERM);
    LxtCheckErrnoFailure(remove(VFS_ACCESS_STICKY_BIT_DIR "/dir1"), EPERM);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    _exit(Result);
}

int VfsAccessStickyBit(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int ChildStatus;
    int File = 0;
    int Result;

    //
    // Create a directory with the sticky bit set and a file inside.
    //

    LxtCheckErrno(mkdir(VFS_ACCESS_STICKY_BIT_DIR, S_IRWXU | S_ISVTX));
    LxtCheckErrno(mkdir(VFS_ACCESS_STICKY_BIT_DIR "/dir1", S_IRWXU));
    LxtCheckErrno(File = creat(VFS_ACCESS_STICKY_BIT_DIR "/file1", S_IRWXU));
    ChildPid = fork();
    if (ChildPid == 0)
    {
        VfsAccessRmdirCapChild();
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    ChildPid = fork();
    if (ChildPid == 0)
    {
        VfsAccessRenameCapChild();
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (File > 0)
    {
        close(File);
    }

    rmdir(VFS_ACCESS_STICKY_BIT_DIR "/dir1");
    remove(VFS_ACCESS_STICKY_BIT_DIR "/file1");
    rmdir(VFS_ACCESS_STICKY_BIT_DIR);
    return Result;
}

int VfsAccessSetUserGroupIdExecveChild(void)

/*++

Routine Description:

    This routine runs the child process for VfsAccessSetUserGroupId.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    uid_t EffectiveGroup;
    uid_t FilesystemGroup;
    uid_t RealGroup;
    uid_t SavedGroup;
    int Result = LXT_RESULT_FAILURE;
    uid_t EffectiveUser;
    uid_t FilesystemUser;
    uid_t RealUser;
    uid_t SavedUser;

    LxtLogInfo("Child executable starting");

    //
    // Get the user and group id and verify they match the expected.
    //

    LxtCheckResult(getresuid(&RealUser, &EffectiveUser, &SavedUser));
    LxtCheckEqual(EffectiveUser, VFS_ACCESS_UID, "%u");
    LxtCheckEqual(SavedUser, VFS_ACCESS_UID, "%u");

    FilesystemUser = LxtSetfsuid(-1);
    LxtCheckEqual(FilesystemUser, VFS_ACCESS_UID, "%u");

    LxtCheckResult(getresgid(&RealGroup, &EffectiveGroup, &SavedGroup));
    LxtCheckEqual(EffectiveGroup, VFS_ACCESS_UID, "%u");
    LxtCheckEqual(SavedGroup, VFS_ACCESS_UID, "%u");

    FilesystemGroup = LxtSetfsgid(-1);
    LxtCheckEqual(FilesystemGroup, VFS_ACCESS_UID, "%u");

    LxtLogInfo("Child executable finished");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void VfsAccessSetUserGroupIdFSetIdChild(int Fd1)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    char* Data = "Test data";
    int Fd2 = -1;
    char Path[PATH_MAX];
    int Result;

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;

    //
    // Drop privileges so the current process does not have VFS capabilities
    // and is in the other user\group.
    //

    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(setgid(VFS_ACCESS_UID));
    LxtCheckErrno(setuid(VFS_ACCESS_UID));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    // The plan 9 server cannot know about the uid change after open, so reopen
    // the file with the new security context.
    if (g_LxtFsInfo.FsType == LxtFsTypePlan9)
    {
        snprintf(Path, sizeof(Path), "/proc/self/fd/%d", Fd1);
        LxtCheckResult(Fd2 = open(Path, O_WRONLY));
        LxtCheckErrno(write(Fd2, Data, 1));
        LxtCheckClose(Fd2);
    }
    else
    {
        LxtCheckErrno(write(Fd1, Data, 1));
    }
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd2 >= 0)
    {
        close(Fd2);
    }

    _exit(Result);
}

void VfsAccessSetUserGroupIdChmodChild(char* FilePath, uid_t Uid, gid_t Gid)

/*++
--*/

{

    struct stat Buffer;
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int Result;

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
    CapData[CAP_TO_INDEX(CAP_CHOWN)].permitted |= CAP_TO_MASK(CAP_CHOWN);
    // CapData[CAP_TO_INDEX(CAP_FOWNER)].permitted |= CAP_TO_MASK(CAP_FOWNER);
    CapData[0].effective = CapData[0].permitted;
    CapData[1].effective = CapData[1].permitted;
    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    LxtLogInfo("chown(%s, %d, %d)", FilePath, Uid, Gid);
    LxtCheckErrnoFailure(chown(FilePath, Uid, Gid), EPERM);
    LxtCheckErrno(stat(FilePath, &Buffer));
    LxtCheckEqual((Buffer.st_mode & (S_ISUID | S_ISGID)), (S_ISUID | S_ISGID), "%o");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    _exit(Result);
}

int VfsAccessSetUserGroupId(PLXT_ARGS Args)

/*++
--*/

{

    char* Argv[4];
    struct stat Buffer;
    int ChildPid;
    char* Envp[1];
    mode_t ExpectedMode;
    int Fd1 = 0;
    int Fd2 = 0;
    int Mode;
    int Result;

    rmdir(VFS_ACCESS_GROUP_USER_ID_DIR);

    //
    // Create a directory with the set-group-ID bit set.
    //

    LxtCheckErrno(mkdir(VFS_ACCESS_GROUP_USER_ID_DIR, S_IRWXU));
    LxtCheckErrno(stat(VFS_ACCESS_GROUP_USER_ID_DIR, &Buffer));
    LxtLogInfo("VFS_ACCESS_GROUP_USER_ID_DIR mode after mkdir %o", Buffer.st_mode);

    //
    // Change the owner of the directory and set the set-group-ID bit.
    //

    LxtCheckErrno(chown(VFS_ACCESS_GROUP_USER_ID_DIR, VFS_ACCESS_UID, VFS_ACCESS_UID));
    LxtCheckErrno(chmod(VFS_ACCESS_GROUP_USER_ID_DIR, S_IRWXU | S_ISGID));
    LxtCheckErrno(stat(VFS_ACCESS_GROUP_USER_ID_DIR, &Buffer));
    LxtLogInfo("VFS_ACCESS_GROUP_USER_ID_DIR mode after chmod %o", Buffer.st_mode);

    //
    // Create some files and child directories.
    //

    LxtCheckErrno(Fd1 = creat(VFS_ACCESS_GROUP_USER_ID_DIR "/file1", 0777 | S_IRWXU | S_ISGID | S_ISUID));
    LxtCheckErrno(stat(VFS_ACCESS_GROUP_USER_ID_DIR "/file1", &Buffer));
    LxtLogInfo("VFS_ACCESS_GROUP_USER_ID_DIR /file1 mode after mkdir %o", Buffer.st_mode);

    LxtCheckErrno(mkdir(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1", S_IRWXU));
    LxtCheckErrno(Fd2 = creat(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1/file2", S_IRWXU));
    LxtCheckErrno(mkdir(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1/dir2", S_IRWXU));

    //
    // Validate the files and directories have the correct uid and gid.
    //

    LxtCheckErrno(stat(VFS_ACCESS_GROUP_USER_ID_DIR "/file1", &Buffer));
    if (Buffer.st_gid != VFS_ACCESS_UID)
    {
        LxtLogError("/file1 gid %u does not match expected %u", Buffer.st_gid, VFS_ACCESS_UID);
    }

    LxtCheckErrno(stat(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1", &Buffer));
    if (Buffer.st_gid != VFS_ACCESS_UID)
    {
        LxtLogError("/dir gid %u does not match expected %u", Buffer.st_gid, VFS_ACCESS_UID);
    }

    LxtCheckErrno(stat(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1/file2", &Buffer));
    if (Buffer.st_gid != VFS_ACCESS_UID)
    {
        LxtLogError("/dir1/file2 gid %u does not match expected %u", Buffer.st_gid, VFS_ACCESS_UID);
    }

    LxtCheckErrno(stat(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1/dir2", &Buffer));
    if (Buffer.st_gid != VFS_ACCESS_UID)
    {
        LxtLogError("/dir1/dir2 gid %u does not match expected %u", Buffer.st_gid, VFS_ACCESS_UID);
    }

    //
    // Validate the execute behavior of the set user id and group id bits.
    // Make a copy of the current binary to use for the test.
    //

    // change Args->Argv[0] so that it points to the new single test binary design
    Args->Argv[0] = WSL_UNIT_TEST_BINARY;

    LxtCheckResult(LxtCopyFile(Args->Argv[0], VFS_ACCESS_PARENT_DIR "/wsl_unit_tests"));

    LxtCheckErrno(chown(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", VFS_ACCESS_UID, VFS_ACCESS_UID));

    LxtCheckErrno(stat(Args->Argv[0], &Buffer));
    LxtCheckErrno(chmod(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", (Buffer.st_mode | S_ISUID | S_ISGID)));

    ExpectedMode = Buffer.st_mode;

    //
    // Start the child process.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        Argv[0] = VFS_ACCESS_PARENT_DIR "/wsl_unit_tests";
        Argv[1] = "vfsaccess";
        Argv[2] = "-c";
        Argv[3] = Envp[0] = NULL;
        LxtCheckErrno(stat(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", &Buffer));
        LxtLogInfo("child %o %u %u", Buffer.st_mode, Buffer.st_uid, Buffer.st_gid);
        execve(Argv[0], Argv, Envp);
        LxtLogError("Execve failed, errno: %d (%s)", errno, strerror(errno));
        _exit(LXT_RESULT_FAILURE);
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Set the uid and gid again to make sure the set-user-id and set-group-id
    // bits are stripped from the mode.
    //

    LxtCheckErrno(chown(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", -1, -1));
    LxtCheckErrno(stat(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", &Buffer));
    LxtCheckEqual(Buffer.st_mode, ExpectedMode, "0%o");

    //
    // Re-set the set-user-id and set-group-id bits.
    //

    LxtCheckErrno(chmod(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", (Buffer.st_mode | S_ISUID | S_ISGID)));

    // VirtioFs does not currently handle capability flags. There is a new KILLPRIV2 FUSE flag that may address this in the future.
    if (g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
    {
        //
        // Fork and drop privileges so the current process does not have CAP_FOWNER
        // which is required for changing the owner of a file with the set-user-id
        // or set-group-id bits set.
        //

        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            VfsAccessSetUserGroupIdChmodChild(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", VFS_ACCESS_UID, VFS_ACCESS_UID);
        }

        LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            VfsAccessSetUserGroupIdChmodChild(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", VFS_ACCESS_UID, -1);
        }

        LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            VfsAccessSetUserGroupIdChmodChild(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", -1, VFS_ACCESS_UID);
        }

        LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {
            VfsAccessSetUserGroupIdChmodChild(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests", -1, -1);
        }

        LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    }

    //
    // Validate the behavior of CAP_FSETID for files.
    //

    LxtLogInfo("Checking CAP_FSETID for files");
    LxtCheckErrno(fstat(Fd1, &Buffer));
    Mode = Buffer.st_mode;
    if ((Mode & (S_ISGID | S_ISUID)) != (S_ISGID | S_ISUID))
    {
        LxtLogError("Unexpected mode");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckErrno(write(Fd1, &Buffer, sizeof(Buffer)));
    LxtCheckErrno(fstat(Fd1, &Buffer));
    LxtCheckEqual(Buffer.st_mode, Mode, "%o");

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        VfsAccessSetUserGroupIdFSetIdChild(Fd1);
    }

    //
    // Wait for the child to exit and validate that the set id bits were
    // silently removed.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    LxtCheckErrno(fstat(Fd1, &Buffer));
    LxtCheckEqual(Buffer.st_mode, Mode & ~(S_ISGID | S_ISUID), "%o");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd1 > 0)
    {
        LxtClose(Fd1);
    }

    if (Fd2 > 0)
    {
        LxtClose(Fd2);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    //
    // Clean-up created files and directories.
    //

    unlink(VFS_ACCESS_PARENT_DIR "/wsl_unit_tests");
    remove(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1/file2");
    rmdir(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1/dir2");
    remove(VFS_ACCESS_GROUP_USER_ID_DIR "/file1");
    rmdir(VFS_ACCESS_GROUP_USER_ID_DIR "/dir1");
    rmdir(VFS_ACCESS_GROUP_USER_ID_DIR);
    return Result;
}

void VfsAccessInodeChecksChild(void)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    char* CommandLine[] = {NULL, NULL};
    int ErrnoExpected;
    char FileName[100];
    unsigned int Index;
    int Result;
    int ResultActual;
    int ResultExpected;

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;

    //
    // Drop privileges so the current process does not have VFS capabilities
    // and is in the other user\group.
    //

    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(setgid(VFS_ACCESS_UID));
    LxtCheckErrno(setuid(VFS_ACCESS_UID));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    //
    // For each file, check that read, write and execute is enforced. Similarly
    // for directories check that list, create\delete, and search is enforced.
    //

    for (Index = 0; Index < LXT_COUNT_OF(g_VfsInodeEntries); ++Index)
    {
        if (S_ISREG(g_VfsInodeEntries[Index].Mode))
        {

            //
            // Check read access.
            //

            ResultExpected = -1;
            if ((g_VfsInodeEntries[Index].Mode & S_IROTH) != 0)
            {
                ResultExpected = 0;
            }

            ResultActual = open(g_VfsInodeEntries[Index].Name, O_RDONLY, 0);
            if (ResultActual != -1)
            {
                LxtClose(ResultActual);
                ResultActual = 0;
            }

            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EACCES, "file open O_RDONLY", Index));

            //
            // Check write access.
            //

            ResultExpected = -1;
            if ((g_VfsInodeEntries[Index].Mode & S_IWOTH) != 0)
            {
                ResultExpected = 0;
            }

            ResultActual = open(g_VfsInodeEntries[Index].Name, O_WRONLY, 0);
            if (ResultActual != -1)
            {
                LxtClose(ResultActual);
                ResultActual = 0;
            }

            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EACCES, "file open O_WRONLY", Index));

            //
            // Check read\write access.
            //

            ResultExpected = -1;
            if (((g_VfsInodeEntries[Index].Mode & S_IROTH) != 0) && ((g_VfsInodeEntries[Index].Mode & S_IWOTH) != 0))
            {

                ResultExpected = 0;
            }

            ResultActual = open(g_VfsInodeEntries[Index].Name, O_RDWR, 0);
            if (ResultActual != -1)
            {
                LxtClose(ResultActual);
                ResultActual = 0;
            }

            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EACCES, "file open O_RDWR", Index));

            //
            // Check no access (open time check for read\write).
            //

            if (((g_VfsInodeEntries[Index].Mode & S_IROTH) != 0) && ((g_VfsInodeEntries[Index].Mode & S_IWOTH) != 0))
            {

                ResultExpected = 0;
            }

            ResultActual = open(g_VfsInodeEntries[Index].Name, O_NOACCESS, 0);
            if (ResultActual != -1)
            {
                LxtClose(ResultActual);
                ResultActual = 0;
            }

            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EACCES, "file open O_NOACCESS", Index));

            //
            // Check execute access.
            //

            ErrnoExpected = EACCES;
            ResultExpected = -1;
            if ((g_VfsInodeEntries[Index].Mode & S_IXOTH) != 0)
            {
                ErrnoExpected = ENOEXEC;
            }

            CommandLine[0] = g_VfsInodeEntries[Index].Name;
            ResultActual = execv(CommandLine[0], CommandLine);
            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "execv", Index));
        }
        else
        {

            //
            // Check read access.
            //

            ResultExpected = -1;
            if ((g_VfsInodeEntries[Index].Mode & S_IROTH) != 0)
            {
                ResultExpected = 0;
            }

            ResultActual = open(g_VfsInodeEntries[Index].Name, O_RDONLY, 0);
            if (ResultActual != -1)
            {
                LxtClose(ResultActual);
                ResultActual = 0;
            }

            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EACCES, "directory open O_RDONLY", Index));

            //
            // Check create\delete (write) access. Execute access is also
            // required to create and delete.
            //

            sprintf(FileName, "%s/%s", g_VfsInodeEntries[Index].Name, VFS_ACCESS_INODE_ENTRY_FILE);

            ResultExpected = -1;
            if (((g_VfsInodeEntries[Index].Mode & S_IWOTH) != 0) && ((g_VfsInodeEntries[Index].Mode & S_IXOTH) != 0))
            {

                ResultExpected = 0;
            }

            ResultActual = open(FileName, O_CREAT | O_RDONLY, S_IRUSR);
            if (ResultActual != -1)
            {
                LxtClose(ResultActual);
                ResultActual = 0;
            }

            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EACCES, "directory create file", Index));

            if (ResultActual == 0)
            {
                ResultActual = unlink(FileName);
                LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, EACCES, "directory delete file", Index));
            }

            //
            // Check search (execute) access.
            //

            ErrnoExpected = EACCES;
            ResultExpected = -1;
            if ((g_VfsInodeEntries[Index].Mode & S_IXOTH) != 0)
            {
                ErrnoExpected = ENOENT;
            }

            ResultActual = open(FileName, O_RDONLY, 0);
            if (ResultActual != -1)
            {
                LxtClose(ResultActual);
                ResultActual = 0;
            }

            LxtCheckResult(VfsAccessCheckResult(ResultActual, ResultExpected, errno, ErrnoExpected, "directory search file", Index));
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    _exit(Result);
}

int VfsAccessInodeChecks(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int Result;

    ChildPid = fork();
    if (ChildPid == 0)
    {
        VfsAccessInodeChecksChild();
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessParseArgs(int Argc, char* Argv[], LXT_ARGS* Args)

/*++

Routine Description:

    This routine parses command line arguments for the vfsaccess tests.

Arguments:

    Argc - Supplies the number of arguments.

    Argv - Supplies an array of arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ArgvIndex;
    bool Cleanup;
    const char* Name;
    int Result;
    int ValidArguments;

    Result = LXT_RESULT_FAILURE;
    ValidArguments = 0;
    g_UseDrvFs = false;
    Name = LXT_NAME;
    Cleanup = true;
    if (Argc < 1)
    {
        goto ErrorExit;
    }

    umask(0);
    for (ArgvIndex = 1; ArgvIndex < Argc; ++ArgvIndex)
    {
        if (strcmp(Argv[ArgvIndex], "drvfs") == 0)
        {
            g_UseDrvFs = true;
            Name = LXT_NAME_DRVFS;
            continue;
        }

        if (Argv[ArgvIndex][0] != '-')
        {
            printf("Unexpected character %s", Argv[ArgvIndex]);
            goto ErrorExit;
        }

        switch (Argv[ArgvIndex][1])
        {
        case 'c':

            //
            // Run the setusergroupid execve test child
            //

            ValidArguments = 1;
            Cleanup = false;
            Result = VfsAccessSetUserGroupIdExecveChild();
            goto ErrorExit;

        case 'v':
        case 'l':

            //
            // This was already taken care of by LxtInitialize.
            //

            ++ArgvIndex;

            break;

        case 'h':
        case 'a':
            break;

        default:
            goto ErrorExit;
        }
    }

    //
    // If -c was not specified, just run the tests
    //

    ValidArguments = 1;
    LxtCheckResult(LxtInitialize(Argc, Argv, Args, Name));
    LxtCheckResult(LxtFsTestSetup(Args, VFS_ACCESS_PARENT_DIR, "/vfsaccesstest", g_UseDrvFs));

    if (Args->HelpRequested == false)
    {
        LxtLogInfo("Creating files.");
        LxtCheckResult(VfsAccessFileObjectCreateFiles());
        LxtCheckResult(VfsAccessFileObjectCreateSymlinks());
    }

    //
    // Tests must be run forked since some of the tests change the uid and
    // don't change it back, which breaks umount during cleanup.
    //

    LxtCheckResult(LxtRunVariationsForked(Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    if (ValidArguments == 0)
    {
        printf("\nuse: %s <One of the below arguments>\n", Argv[0]);
        printf("\t-c : Run %s execve test child (don't use directly)\n", Argv[0]);
    }

    if (Cleanup != false)
    {
        VfsAccessFileObjectCleanup();
        LxtFsTestCleanup(VFS_ACCESS_PARENT_DIR, "/vfsaccesstest", g_UseDrvFs);
    }

    return Result;
}

void VfsAccessUTimeCapChild(void)

/*++
--*/

{

    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int Fd;
    int Result;
    struct timeval Times[2];

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
    CapData[CAP_TO_INDEX(CAP_DAC_OVERRIDE)].permitted |= CAP_TO_MASK(CAP_DAC_OVERRIDE);
    CapData[CAP_TO_INDEX(CAP_CHOWN)].permitted |= CAP_TO_MASK(CAP_CHOWN);
    CapData[0].effective = CapData[0].permitted;
    CapData[1].effective = CapData[1].permitted;

    //
    // Drop privileges so the current process does not have CAP_FOWNER.
    //

    LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
    LxtCheckErrno(setgid(VFS_ACCESS_UID));
    LxtCheckErrno(setuid(VFS_ACCESS_UID));
    LxtCheckErrno(LxtCapSet(&CapHeader, CapData));

    //
    // Create a file with a different user.
    //

    unlink(VFS_ACCESS_UTIME_FILE);
    LxtCheckErrno(Fd = open(VFS_ACCESS_UTIME_FILE, O_CREAT, 0));
    LxtClose(Fd);
    LxtCheckErrno(chown(VFS_ACCESS_UTIME_FILE, VFS_ACCESS_UID + 1, VFS_ACCESS_UID + 1));

    //
    // Try to change the time on the file to 0.
    //

    memset(Times, 0, sizeof(Times));
    LxtCheckErrnoFailure(utimes(VFS_ACCESS_UTIME_FILE, Times), EPERM);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    unlink(VFS_ACCESS_UTIME_FILE);
    _exit(Result);
}

int VfsAccessUTimeCap(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    int Result;

    ChildPid = fork();
    if (ChildPid == 0)
    {
        VfsAccessUTimeCapChild();
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int VfsAccessSetFsUid(PLXT_ARGS Args)

/*++
--*/

{

    unsigned long long Effective;
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    struct __user_cap_data_struct ExpectedCapData[2];
    int Fd;
    int Index;
    struct passwd* Password;
    int Result;

    memset(&CapData, 0, sizeof(CapData));
    memset(&CapHeader, 0, sizeof(CapHeader));
    CapHeader.version = _LINUX_CAPABILITY_VERSION_3;

    Fd = -1;
    Result = LXT_RESULT_FAILURE;

    //
    // Get the password entry for the 'nobody' user.
    //

    Password = getpwnam("nobody");
    if (Password == NULL)
    {
        goto ErrorExit;
    }

    //
    // Create a file to be used for access checks.
    //

    Fd = open(VFS_ACCESS_FSUID_FILE, O_CREAT | O_RDWR, 0644);
    if (Fd < 0)
    {
        goto ErrorExit;
    }

    //
    // Get the original capabilities.
    //

    LxtCheckErrno(LxtCapGet(&CapHeader, ExpectedCapData));
    Effective = (((unsigned long long)ExpectedCapData[0].effective) << 32) | ExpectedCapData[1].effective;
    LxtLogInfo("Before setfsuid(nobody) %016llX", Effective);

    //
    // Set the fsuid and ensure that the correct capabilities are dropped when
    // switching from root.
    //

    if (LxtSetfsuid(Password->pw_uid) < 0)
    {
        goto ErrorExit;
    }

    LxtCheckErrno(LxtCapGet(&CapHeader, CapData));
    Effective = (((unsigned long long)CapData[0].effective) << 32) | CapData[1].effective;
    LxtLogInfo("After setfsuid(nobody) %016llX", Effective);
    for (Index = 0; Index < LXT_COUNT_OF(g_VfsSetFsUidCaps); Index++)
    {
        ExpectedCapData[CAP_TO_INDEX(g_VfsSetFsUidCaps[Index])].effective &= ~CAP_TO_MASK(g_VfsSetFsUidCaps[Index]);
    }

    if ((CapData[0].effective != ExpectedCapData[0].effective) || (CapData[1].effective != ExpectedCapData[1].effective))
    {

        LxtLogError("Capabilities do not match expected");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // Verify that opening the file fails since we no longer have the correct fsuid or capabilities.
    //

    LxtCheckErrnoFailure(open(VFS_ACCESS_FSUID_FILE, O_RDWR), EACCES);

    //
    // Set the fsuid back to root and verify that the capabilities were correctly restored.
    //

    LxtCheckErrno(LxtSetfsuid(0));

    LxtCheckErrno(LxtCapGet(&CapHeader, CapData));
    Effective = (((unsigned long long)CapData[0].effective) << 32) | CapData[1].effective;
    LxtLogInfo("After setfsuid(root) %016llX", Effective);
    for (Index = 0; Index < LXT_COUNT_OF(g_VfsSetFsUidCaps); Index++)
    {
        ExpectedCapData[CAP_TO_INDEX(g_VfsSetFsUidCaps[Index])].effective |= CAP_TO_MASK(g_VfsSetFsUidCaps[Index]);
    }

    if ((CapData[0].effective != ExpectedCapData[0].effective) || (CapData[1].effective != ExpectedCapData[1].effective))
    {

        LxtLogError("Capabilities do not match expected");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    unlink(VFS_ACCESS_FSUID_FILE);
    return Result;
}

void getreuid(struct reuid_t* Set)
{
    getresuid(&Set->r, &Set->e, &Set->s);
    // LxtLogInfo("getresuid(%d,%d,%d)",Set->r,Set->e,Set->s);
}

pid_t fork_wait()
{
    pid_t pid;
    int status = 0;
    if ((pid = fork()) == 0)
    {
        return pid;
    }
    else
    {
        waitpid(pid, &status, 0);
    }
    return pid;
}

int VfsAccessSetUid(PLXT_ARGS Args)

{

    struct reuid_t Original;
    struct reuid_t Set;
    struct passwd* Nobody;
    int Result;

    getreuid(&Original);
    LxtLogInfo("Current UID: %d", Original.r);
    LxtLogInfo("Current EUID: %d", Original.e);
    LxtLogInfo("Current SUID: %d", Original.s);

    //
    // Try setting without changing
    //

    setreuid(-1, -1);
    getreuid(&Set);
    LxtCheckEqual(Set.r, Original.r, "%d");
    LxtCheckEqual(Set.e, Original.e, "%d");
    LxtCheckEqual(Set.s, Original.s, "%d");

    //
    // More tests possible when run as root.
    //

    if (Original.r == 0 || Original.e == 0)
    {
        Nobody = getpwnam("nobody");
        if (Nobody == NULL)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("Couldn't get details for user 'nobody'");
            goto ErrorExit;
        }

        LxtLogInfo("Attempting setreuid(%d, -1)", Nobody->pw_uid);
        LxtCheckResult(setreuid(Nobody->pw_uid, -1));
        getreuid(&Set);
        if (Set.r != Nobody->pw_uid || Set.e != 0 || Set.s != 0)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }

        //
        // reset state to 0, 0, 0
        //

        LxtLogInfo("setuid(0)");
        LxtCheckResult(setuid(0));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != 0 || Set.s != 0)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            return -1; // Fatal, Nobody tests rely on this succeeding
        }

        //
        // This test checks that setuid only touches the ruid and suid values.
        //

        LxtLogInfo("setresuid(-1, %d, %d)", Nobody->pw_uid, Nobody->pw_uid);
        LxtCheckResult(setresuid(-1, Nobody->pw_uid, Nobody->pw_uid));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != Nobody->pw_uid || Set.s != Nobody->pw_uid)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }

        //
        // Set state to 0, 0, 65534
        //

        LxtLogInfo("Attempting setuid(0)");
        LxtCheckResult(setuid(0));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != 0 || Set.s != 65534)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }

        //
        // This test checks the first transitive property of setreuid wherein
        // setting the effective uid also sets the suid
        //
        // Reset state to 0, 0, 0
        //

        LxtLogInfo("setresuid(0, 0, 0)");
        LxtCheckResult(setresuid(0, 0, 0));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != 0 || Set.s != 0)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            return -1; // Fatal, Other tests rely on this succeeding
        }
        LxtLogInfo("setreuid(-1, %d)", Nobody->pw_uid);
        LxtCheckResult(setreuid(-1, Nobody->pw_uid));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != Nobody->pw_uid || Set.s != Nobody->pw_uid)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }

        //
        // Set state to 0, 0, 65534
        //

        LxtLogInfo("Attempting setuid(0)");
        LxtCheckResult(setuid(0));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != 0 || Set.s != 65534)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }

        //
        // This test checks the second transitive property of setreuid
        // wherein setting the ruid, but not the euid will Set the suid
        // to be the euid
        //

        LxtLogInfo("setresuid(%d, 0, VFS_ACCESS_UID)", Nobody->pw_uid);
        LxtCheckResult(setresuid(Nobody->pw_uid, 0, VFS_ACCESS_UID));
        getreuid(&Set);
        if (Set.r != Nobody->pw_uid || Set.e != 0 || Set.s != VFS_ACCESS_UID)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }
        LxtLogInfo("Attempting setreuid(0, -1)");
        LxtCheckResult(setreuid(0, -1));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != 0 || Set.s != 0)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }

        //
        // This test checks that unprivileged processes can Set the euid
        // to the ruid or suid. Need to fork and wait as privileges are
        // irreversibly dropped by this syscall
        //

        if (fork_wait() == 0)
        {
            LxtLogInfo("setresuid(%d, VFS_ACCESS_UID, 0)", Nobody->pw_uid);
            LxtCheckResult(setresuid(Nobody->pw_uid, VFS_ACCESS_UID, 0));
            getreuid(&Set);
            if (Set.r != Nobody->pw_uid || Set.e != VFS_ACCESS_UID || Set.s != 0)
            {
                LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            }
            LxtLogInfo("Attempting setreuid(-1, %d)", Nobody->pw_uid);
            LxtCheckResult(setreuid(-1, Nobody->pw_uid));
            getreuid(&Set);
            if (Set.r != Nobody->pw_uid || Set.e != Nobody->pw_uid || Set.s != 0)
            {
                LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            }
            exit(0);
        }

        //
        // This test checks that unprivileged processes can Set the euid
        // to the ruid or suid.
        //

        LxtLogInfo("setresuid(%d, %d, 0)", Nobody->pw_uid, Nobody->pw_uid);
        LxtCheckResult(setresuid(Nobody->pw_uid, Nobody->pw_uid, 0));
        getreuid(&Set);
        if (Set.r != Nobody->pw_uid || Set.e != Nobody->pw_uid || Set.s != 0)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }
        LxtLogInfo("Attempting setreuid(-1, 0)");
        LxtCheckResult(setreuid(-1, 0));
        getreuid(&Set);
        if (Set.r != Nobody->pw_uid || Set.e != 0 || Set.s != 0)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
        }

        //
        // Reset state to 0, 0, 0
        //

        LxtLogInfo("setresuid(0, 0, 0)");
        LxtCheckResult(setresuid(0, 0, 0));
        getreuid(&Set);
        if (Set.r != 0 || Set.e != 0 || Set.s != 0)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            return -1; // Fatal, Other tests rely on this succeeding
        }

        //
        // This test validates that unprivileged users can only Set the ruid
        // to the ruid or the euid
        //

        if (fork_wait() == 0)
        {
            LxtLogInfo("setresuid(%d, VFS_ACCESS_UID, 0)", Nobody->pw_uid);
            LxtCheckResult(setresuid(Nobody->pw_uid, VFS_ACCESS_UID, 0));
            getreuid(&Set);
            if (Set.r != Nobody->pw_uid || Set.e != VFS_ACCESS_UID || Set.s != 0)
            {
                LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            }
            LxtLogInfo("Attempting setreuid(VFS_ACCESS_UID, -1)");
            LxtCheckResult(setreuid(VFS_ACCESS_UID, -1));
            getreuid(&Set);
            if (Set.r != VFS_ACCESS_UID || Set.e != VFS_ACCESS_UID || Set.s != VFS_ACCESS_UID)
            {
                LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            }
            exit(0);
        }
        if (fork_wait() == 0)
        {
            LxtLogInfo("setresuid(%d, VFS_ACCESS_UID, 0)", Nobody->pw_uid);
            LxtCheckResult(setresuid(Nobody->pw_uid, VFS_ACCESS_UID, 0));
            getreuid(&Set);
            if (Set.r != Nobody->pw_uid || Set.e != VFS_ACCESS_UID || Set.s != 0)
            {
                LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            }
            LxtLogInfo("Attempting setreuid(0, -1)");
            LxtCheckErrnoFailure(setreuid(0, -1), EPERM);
            getreuid(&Set);
            if (Set.r != Nobody->pw_uid || Set.e != VFS_ACCESS_UID || Set.s != 0)
            {
                LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            }
            exit(0);
        }

        //
        // Drop all permissions permanently
        //

        LxtLogInfo("Dropping all permissions");
        LxtLogInfo("setresuid(%d, %d, %d)", Nobody->pw_uid, Nobody->pw_uid, Nobody->pw_uid);
        LxtCheckResult(setresuid(Nobody->pw_uid, Nobody->pw_uid, Nobody->pw_uid));
        getreuid(&Set);
        if (Set.r != Nobody->pw_uid || Set.e != Nobody->pw_uid)
        {
            LxtLogError("uid=%d, euid=%d, suid=%d", Set.r, Set.e, Set.s);
            return -1;
        }
    }

    //
    // Try to gain root uid
    //

    LxtLogInfo("Attempting setreuid(0, -1)");
    LxtCheckErrnoFailure(setreuid(0, -1), EPERM);
    getreuid(&Set);
    if (Set.r == 0 || Set.e == 0)
    {
        LxtLogError("Gained root permissions!");
    }
    LxtLogInfo("Attempting setreuid(-1, 0)");
    LxtCheckErrnoFailure(setreuid(-1, 0), EPERM);
    getreuid(&Set);
    if (Set.r == 0 || Set.e == 0)
    {
        LxtLogError("Gained root permissions!");
    }
    LxtLogInfo("Attempting setreuid(0, 0)");
    LxtCheckErrnoFailure(setreuid(0, 0), EPERM);
    getreuid(&Set);
    if (Set.r == 0 || Set.e == 0)
    {
        LxtLogError("Gained root permissions!");
    }

ErrorExit:
    return Result;
}
