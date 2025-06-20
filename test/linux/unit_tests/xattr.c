/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Xattr.c

Abstract:

    This file is a xattr test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include "lxtutil.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/user.h>
#include <stddef.h>
#include <fcntl.h>
#include "lxtfs.h"
#include <sys/xattr.h>
#include <libmount/libmount.h>
#include "lxtmount.h"

#if !defined(__amd64__) && !defined(__aarch64__)

#include <sys/capability.h>

#else

#include <sys/cdefs.h>
#include <linux/capability.h>

#define _LINUX_CAPABILITY_VERSION_3 0x20080522

#endif

#define LXT_NAME "xattr"
#define LXT_NAME_DRVFS "xattr_drvfs"

#define LXT_XATTR_MODE 0777
#define LXT_XATTR_UID 1004
#define LXT_XATTR_GID 1004
#define LXT_XATTR_TEST_PARENT "/data/xattrtest"
#define LXT_XATTR_ACCESS_FILE_PATH LXT_XATTR_TEST_PARENT "/xattrAccessFile"
#define LXT_XATTR_FILE_PATH LXT_XATTR_TEST_PARENT "/xattrFile"
#define LXT_XATTR_DIR_PATH LXT_XATTR_TEST_PARENT "/xattrDir"
#define LXT_XATTR_LINK_PATH LXT_XATTR_TEST_PARENT "/xattrLink"
#define LXT_XATTR_FIFO_PATH LXT_XATTR_TEST_PARENT "/xattrFifo"
#define LXT_XATTR_SIZE_MAX (4040)
#define LXT_XATTR_CASE_SENSITIVE_LENGTH (sizeof(LXT_XATTR_CASE_SENSITIVE) - 1)
#define LXT_XATTR_TEST_VALUE "test"
#define LXT_XATTR_TEST_LENGTH (sizeof(LXT_XATTR_TEST_VALUE) - 1)

#define LXT_XATTR_PATH_COUNT (LXT_COUNT_OF(g_XattrPaths))

int XattrTestCreatePaths(int* Fds);

void XattrTestDeletePaths(int* Fds);

LXT_VARIATION_HANDLER XattrAccessTest;
LXT_VARIATION_HANDLER XattrListTest;
LXT_VARIATION_HANDLER XattrGetTest;
LXT_VARIATION_HANDLER XattrRemoveTest;
LXT_VARIATION_HANDLER XattrSetTest;

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"xattr list", XattrListTest}, {"xattr get", XattrGetTest}, {"xattr set", XattrSetTest}, {"xattr remove", XattrRemoveTest}, {"xattr access", XattrAccessTest}};

static const char* g_XattrPaths[] = {
    LXT_XATTR_FILE_PATH, LXT_XATTR_DIR_PATH, LXT_XATTR_LINK_PATH, LXT_XATTR_FIFO_PATH, "/dev/null", "/proc/cpuinfo"};

ssize_t GetXattr(const char* Path, const char* Name, void* Value, size_t Size)
{
    size_t Result = LxtGetxattr(Path, Name, Value, Size);
    if (Result < 0)
    {
        int SavedErrno = errno;
        ssize_t SavedResult = Result;
        Result = LxtGetxattr(Path, Name, NULL, 0);
        LxtLogInfo("getxattr(%s, %s, NULL, 0) = %Iu", Path, Name, Result);
        errno = SavedErrno;
        Result = SavedResult;
    }

    return Result;
}

int XattrTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Index;
    const char* Name;
    int Result;
    bool UseDrvFs;

    Name = LXT_NAME;
    UseDrvFs = false;
    for (Index = 1; Index < Argc; Index += 1)
    {
        if (strcmp(Argv[Index], "drvfs") == 0)
        {
            UseDrvFs = true;
            Name = LXT_NAME_DRVFS;
            break;
        }
    }

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, Name));
    LxtCheckResult(LxtFsTestSetup(&Args, LXT_XATTR_TEST_PARENT, "/xattrtest", UseDrvFs));

    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtFsTestCleanup(LXT_XATTR_TEST_PARENT, "/xattrtest", UseDrvFs);
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int XattrTestCreatePaths(int* Fds)

/*++
--*/

{

    int Fd;
    int Index;
    int Result;

    memset(Fds, -1, sizeof(*Fds) * LXT_XATTR_PATH_COUNT);
    XattrTestDeletePaths(NULL);
    LxtCheckErrno(Fd = creat(LXT_XATTR_FILE_PATH, LXT_XATTR_MODE));
    LxtClose(Fd);
    LxtCheckErrno(mkdir(LXT_XATTR_DIR_PATH, LXT_XATTR_MODE));
    LxtCheckErrno(symlink(LXT_XATTR_FILE_PATH, LXT_XATTR_LINK_PATH));
    LxtCheckErrno(mkfifo(LXT_XATTR_FIFO_PATH, LXT_XATTR_MODE));
    for (Index = 0; Index < LXT_XATTR_PATH_COUNT; ++Index)
    {
        Fds[Index] = open(g_XattrPaths[Index], O_RDONLY | O_NONBLOCK);
    }

ErrorExit:
    return Result;
}

void XattrTestDeletePaths(int* Fds)

{

    int Index;

    if (Fds != NULL)
    {
        for (Index = 0; Index < LXT_XATTR_PATH_COUNT; ++Index)
        {
            if (Fds[Index] != -1)
            {
                LxtClose(Fds[Index]);
                Fds[Index] = -1;
            }
        }
    }

    unlink(LXT_XATTR_FILE_PATH);
    rmdir(LXT_XATTR_DIR_PATH);
    unlink(LXT_XATTR_LINK_PATH);
    unlink(LXT_XATTR_FIFO_PATH);
    return;
}

int XattrListTest(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[1024];
    ssize_t ExpectedSize;
    int Fds[LXT_XATTR_PATH_COUNT];
    int Index;
    ssize_t Result;
    ssize_t Size;

    LxtCheckErrno(XattrTestCreatePaths(Fds));
    for (Index = 0; Index < (LXT_XATTR_PATH_COUNT - 1); ++Index)
    {

        LxtLogInfo("%s", g_XattrPaths[Index]);

        //
        // Check that the xattr syscalls return 0 for all entries to indicate no
        // attributes are present.
        //
        // N.B. DrvFs (not in WslFs mode) will return the case sensitivity
        //      attribute for directories only.
        //

        if ((Index == 1) && ((g_LxtFsInfo.FsType == LxtFsTypeDrvFs) || (g_LxtFsInfo.FsType == LxtFsTypeVirtioFs)))
        {
            ExpectedSize = LXT_XATTR_CASE_SENSITIVE_LENGTH + 1;
        }
        else
        {
            ExpectedSize = 0;
        }

        LxtCheckErrno(Size = LxtListxattr(g_XattrPaths[Index], Buffer, sizeof(Buffer)));
        LxtCheckEqual(Size, ExpectedSize, "%lld");
        LxtCheckErrno(Size = LxtLlistxattr(g_XattrPaths[Index], Buffer, sizeof(Buffer)));
        LxtCheckEqual(Size, ExpectedSize, "%lld");
        LxtCheckErrno(Size = LxtFlistxattr(Fds[Index], Buffer, sizeof(Buffer)));
        LxtCheckEqual(Size, ExpectedSize, "%lld");

        //
        // Check that the buffer and size are not validated if there are no
        // attributes.
        //

        if (ExpectedSize == 0)
        {
            LxtCheckErrnoZeroSuccess(LxtListxattr(g_XattrPaths[Index], 0x1, sizeof(Buffer)));
            LxtCheckErrnoZeroSuccess(LxtLlistxattr(g_XattrPaths[Index], 0x1, sizeof(Buffer)));
            LxtCheckErrnoZeroSuccess(LxtFlistxattr(Fds[Index], 0x1, sizeof(Buffer)));
            LxtCheckErrnoZeroSuccess(LxtListxattr(g_XattrPaths[Index], Buffer, -1));
            LxtCheckErrnoZeroSuccess(LxtLlistxattr(g_XattrPaths[Index], Buffer, -1));
            LxtCheckErrnoZeroSuccess(LxtFlistxattr(Fds[Index], Buffer, -1));
        }
    }

    //
    // Check for invalid parameters.
    //

    LxtCheckErrnoFailure(LxtListxattr(0x1, Buffer, sizeof(Buffer)), EFAULT);
    LxtCheckErrnoFailure(LxtLlistxattr(0x1, Buffer, sizeof(Buffer)), EFAULT);
    LxtCheckErrnoFailure(LxtFlistxattr(-1, Buffer, sizeof(Buffer)), EBADF);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    XattrTestDeletePaths(Fds);
    return (int)Result;
}

int XattrGetTest(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[1024];
    int Failures[] = {ENODATA, ENODATA, ENODATA, ENODATA, ENODATA, ENOTSUP, ENOTSUP};
    int Fds[LXT_XATTR_PATH_COUNT];
    int Index;
    char* Name = "security.capability";
    ssize_t Result;

    LxtCheckErrno(XattrTestCreatePaths(Fds));
    for (Index = 0; Index < LXT_XATTR_PATH_COUNT; ++Index)
    {

        //
        // Check that the xattr syscalls return 0 for all entries to indicate no
        // attributes are present.
        //

        LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[Index], Name, Buffer, sizeof(Buffer)), Failures[Index]);
        LxtCheckErrnoFailure(LxtLgetxattr(g_XattrPaths[Index], Name, Buffer, sizeof(Buffer)), Failures[Index]);
        LxtCheckErrnoFailure(LxtFgetxattr(Fds[Index], Name, Buffer, sizeof(Buffer)), Failures[Index]);

        //
        // Check that the buffer and size are not validated if there are no
        // attributes.
        //

        LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[Index], Name, 0x1, sizeof(Buffer)), Failures[Index]);
        LxtCheckErrnoFailure(LxtLgetxattr(g_XattrPaths[Index], Name, 0x1, sizeof(Buffer)), Failures[Index]);
        LxtCheckErrnoFailure(LxtFgetxattr(Fds[Index], Name, 0x1, sizeof(Buffer)), Failures[Index]);
        LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[Index], Name, Buffer, -1), Failures[Index]);
        LxtCheckErrnoFailure(LxtLgetxattr(g_XattrPaths[Index], Name, Buffer, -1), Failures[Index]);
        LxtCheckErrnoFailure(LxtFgetxattr(Fds[Index], Name, Buffer, -1), Failures[Index]);
    }

    //
    // Check for invalid parameters.
    //

    LxtCheckErrnoFailure(LxtGetxattr(0x1, Name, Buffer, sizeof(Buffer)), EFAULT);
    LxtCheckErrnoFailure(LxtLgetxattr(0x1, Name, Buffer, sizeof(Buffer)), EFAULT);
    LxtCheckErrnoFailure(LxtFgetxattr(-1, Name, Buffer, sizeof(Buffer)), EBADF);

    LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[0], 0x1, Buffer, sizeof(Buffer)), EFAULT);
    LxtCheckErrnoFailure(LxtLgetxattr(g_XattrPaths[0], 0x1, Buffer, sizeof(Buffer)), EFAULT);
    LxtCheckErrnoFailure(LxtFgetxattr(Fds[0], 0x1, Buffer, sizeof(Buffer)), EFAULT);

    if (g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
    {
        LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer)), ENOTSUP);
        LxtCheckErrnoFailure(LxtLgetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer)), ENOTSUP);
        LxtCheckErrnoFailure(LxtFgetxattr(Fds[0], "invalid.name", Buffer, sizeof(Buffer)), ENOTSUP);
    }
    else
    {
        // The virtioFs implementation does not restrict the allowed attribute names, but these values will not be present.
        LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer)), ENODATA);
        LxtCheckErrnoFailure(LxtLgetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer)), ENODATA);
        LxtCheckErrnoFailure(LxtFgetxattr(Fds[0], "invalid.name", Buffer, sizeof(Buffer)), ENODATA);
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    XattrTestDeletePaths(Fds);
    return (int)Result;
}

int XattrRemoveTest(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[1024];
    int Fds[LXT_XATTR_PATH_COUNT];
    int Index;
    char* Name = "security.capability";
    int Result;
    char TestData[] = "test";

    LxtCheckErrno(XattrTestCreatePaths(Fds));
    for (Index = 0; Index < LXT_XATTR_PATH_COUNT - 1; ++Index)
    {

        LxtLogInfo("%s %s", g_XattrPaths[Index], Name);

        //
        // Check that the xattr syscalls return the correct error code for all
        // entries to indicate no attributes are present.
        //
        LxtCheckErrnoFailure(LxtRemovexattr(g_XattrPaths[Index], Name), ENODATA);
        LxtCheckErrnoFailure(LxtLremovexattr(g_XattrPaths[Index], Name), ENODATA);
        LxtCheckErrnoFailure(LxtFremovexattr(Fds[Index], Name), ENODATA);
    }

    //
    // Create three ea's and delete them in various orders.
    //

    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.1", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.2", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.3", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.1"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.2"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.3"));

    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.1", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.2", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.3", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.1"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.3"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.2"));

    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.1", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.2", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.3", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.2"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.1"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.3"));

    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.1", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.2", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.3", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.2"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.3"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.1"));

    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.1", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.2", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.3", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.3"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.1"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.2"));

    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.1", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.2", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.3", TestData, sizeof(TestData), 0));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.3"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.2"));
    LxtCheckErrno(LxtRemovexattr(g_XattrPaths[0], "user.1"));

    //
    // Check for invalid parameters.
    //

    LxtCheckErrnoFailure(LxtRemovexattr(0x1, Name), EFAULT);
    LxtCheckErrnoFailure(LxtLremovexattr(0x1, Name), EFAULT);
    LxtCheckErrnoFailure(LxtFremovexattr(-1, Name), EBADF);

    LxtCheckErrnoFailure(LxtRemovexattr(g_XattrPaths[0], 0x1), EFAULT);
    LxtCheckErrnoFailure(LxtLremovexattr(g_XattrPaths[0], 0x1), EFAULT);
    LxtCheckErrnoFailure(LxtFremovexattr(Fds[0], 0x1), EFAULT);

    if (g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
    {
        LxtCheckErrnoFailure(LxtRemovexattr(g_XattrPaths[0], "invalid.name"), ENOTSUP);
        LxtCheckErrnoFailure(LxtLremovexattr(g_XattrPaths[0], "invalid.name"), ENOTSUP);
        LxtCheckErrnoFailure(LxtFremovexattr(Fds[0], "invalid.name"), ENOTSUP);
    }
    else
    {
        // The virtioFs implementation does not restrict the allowed attribute names, but these values will not be present.
        LxtCheckErrnoFailure(LxtRemovexattr(g_XattrPaths[0], "invalid.name"), ENODATA);
        LxtCheckErrnoFailure(LxtLremovexattr(g_XattrPaths[0], "invalid.name"), ENODATA);
        LxtCheckErrnoFailure(LxtFremovexattr(Fds[0], "invalid.name"), ENODATA);
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    XattrTestDeletePaths(Fds);
    return Result;
}

int XattrSetTest(PLXT_ARGS Args)

/*++
--*/

{

    char* AttrName;
    char Buffer[1024];
    int Count;
    char* DynamicBuffer;
    int DynamicBufferSize;
    int Fds[LXT_XATTR_PATH_COUNT];
    int Index;
    char* Name = "security.foo";
    int NameIndex;
    ssize_t Result;
    int Size;
    struct
    {
        char Name[256];
        bool Found;
    } TestXattrs[103];

    int TotalCount;
    int TotalSize;

    DynamicBuffer = NULL;
    LxtCheckErrno(XattrTestCreatePaths(Fds));

    //
    // Check for invalid parameters.
    //

    LxtCheckErrnoFailure(LxtSetxattr(0x1, Name, Buffer, sizeof(Buffer), 0), EFAULT);
    LxtCheckErrnoFailure(LxtLsetxattr(0x1, Name, Buffer, sizeof(Buffer), 0), EFAULT);
    LxtCheckErrnoFailure(LxtFsetxattr(-1, Name, Buffer, sizeof(Buffer), 0), EBADF);

    LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], 0x1, Buffer, sizeof(Buffer), 0), EFAULT);
    LxtCheckErrnoFailure(LxtLsetxattr(g_XattrPaths[0], 0x1, Buffer, sizeof(Buffer), 0), EFAULT);
    LxtCheckErrnoFailure(LxtFsetxattr(Fds[0], 0x1, Buffer, sizeof(Buffer), 0), EFAULT);

    if (g_LxtFsInfo.FsType != LxtFsTypeVirtioFs)
    {
        LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer), 0), ENOTSUP);
        LxtCheckErrnoFailure(LxtLsetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer), 0), ENOTSUP);
        LxtCheckErrnoFailure(LxtFsetxattr(Fds[0], "invalid.name", Buffer, sizeof(Buffer), 0), ENOTSUP);

        LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], "security.", Buffer, sizeof(Buffer), 0), EINVAL);
        LxtCheckErrnoFailure(LxtLsetxattr(g_XattrPaths[0], "security.", Buffer, sizeof(Buffer), 0), EINVAL);
        LxtCheckErrnoFailure(LxtFsetxattr(Fds[0], "security.", Buffer, sizeof(Buffer), 0), EINVAL);

        LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], "trusted.", Buffer, sizeof(Buffer), 0), EINVAL);
        LxtCheckErrnoFailure(LxtLsetxattr(g_XattrPaths[0], "trusted.", Buffer, sizeof(Buffer), 0), EINVAL);
        LxtCheckErrnoFailure(LxtFsetxattr(Fds[0], "trusted.", Buffer, sizeof(Buffer), 0), EINVAL);

        LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], "user.", Buffer, sizeof(Buffer), 0), EINVAL);
        LxtCheckErrnoFailure(LxtLsetxattr(g_XattrPaths[0], "user.", Buffer, sizeof(Buffer), 0), EINVAL);
        LxtCheckErrnoFailure(LxtFsetxattr(Fds[0], "user.", Buffer, sizeof(Buffer), 0), EINVAL);
    }
    else
    {
        // The virtioFs implementation does not restrict the allowed attribute names.
        LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer), 0));
        LxtCheckErrno(LxtLsetxattr(g_XattrPaths[0], "invalid.name", Buffer, sizeof(Buffer), 0));
        LxtCheckErrno(LxtFsetxattr(Fds[0], "invalid.name", Buffer, sizeof(Buffer), 0));
    }

    LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], "system.", Buffer, sizeof(Buffer), 0), ENOTSUP);
    LxtCheckErrnoFailure(LxtLsetxattr(g_XattrPaths[0], "system.", Buffer, sizeof(Buffer), 0), ENOTSUP);
    LxtCheckErrnoFailure(LxtFsetxattr(Fds[0], "system.", Buffer, sizeof(Buffer), 0), ENOTSUP);

    AttrName =
        "user."
        "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
        "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
        "ooooooooooo";
    LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], AttrName, Buffer, sizeof(Buffer), 0), ERANGE);
    LxtCheckErrnoFailure(LxtLsetxattr(g_XattrPaths[0], AttrName, Buffer, sizeof(Buffer), 0), ERANGE);
    LxtCheckErrnoFailure(LxtFsetxattr(Fds[0], AttrName, Buffer, sizeof(Buffer), 0), ERANGE);

    LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], Name, Buffer, sizeof(Buffer), 10), EINVAL);

    //
    // Create an attribute and read it back, using various buffer sizes.
    //

    LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], "user.test", "1234", 4, 0));
    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.test", NULL, 0));
    LxtCheckEqual(Size, 4, "%d");
    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.test", Buffer, sizeof(Buffer)));
    LxtCheckEqual(Size, 4, "%d");
    Buffer[4] = '\0';
    LxtCheckStringEqual(Buffer, "1234");
    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.test", Buffer, 4));
    LxtCheckEqual(Size, 4, "%d");
    Buffer[4] = '\0';
    LxtCheckStringEqual(Buffer, "1234");
    LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[0], "user.test", Buffer, 3), ERANGE);
    LxtCheckErrnoZeroSuccess(LxtRemovexattr(g_XattrPaths[0], "user.test"));

    //
    // Create a max length attribute, and ensure that it exists afterwards.
    //
    // N.B. Max length is based on ext4 limits; LxFs and DrvFs allow bigger
    //      attributes.
    //

    DynamicBuffer = malloc(LXT_XATTR_SIZE_MAX);
    for (Index = 0; Index < LXT_XATTR_SIZE_MAX; Index += 1)
    {
        DynamicBuffer[Index] = (char)Index;
    }

    LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], "user.0", DynamicBuffer, LXT_XATTR_SIZE_MAX, 0));
    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.0", NULL, 0));
    LxtCheckEqual(Size, LXT_XATTR_SIZE_MAX, "%d");
    LxtCheckErrnoZeroSuccess(LxtRemovexattr(g_XattrPaths[0], "user.0"));

    Count = 0;
    memset(TestXattrs, 0, sizeof(TestXattrs));

    //
    // Create a zero length attribute.
    //

    LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], "user.zero", NULL, 0, 0));
    LxtCheckErrnoZeroSuccess(LxtGetxattr(g_XattrPaths[0], "user.zero", NULL, 0));
    strncpy(TestXattrs[Count].Name, "user.zero", sizeof(TestXattrs[Count].Name) - 1);
    Count += 1;

    //
    // Create an attribute with the maximum name length.
    //

    if (g_LxtFsInfo.Flags.DrvFsBehavior != 0)
    {
        AttrName =
            "user."
            "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
            "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
            "oooooooooooooo";
    }
    else
    {
        AttrName =
            "user."
            "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
            "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
            "oooooooooooooooooo";
    }

    LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], AttrName, NULL, 0, 0));
    LxtCheckErrnoZeroSuccess(LxtGetxattr(g_XattrPaths[0], AttrName, NULL, 0));
    strncpy(TestXattrs[Count].Name, AttrName, sizeof(TestXattrs[Count].Name) - 1);
    Count += 1;

    //
    // A bunch of attributes to exercise listing.
    //

    for (int Index = 0; Index < 100; Index += 1)
    {
        snprintf(TestXattrs[Count].Name, sizeof(TestXattrs[Count].Name), "user.test%d", Index);

        LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], TestXattrs[Count].Name, DynamicBuffer, 10, 0));

        Count += 1;
    }

    //
    // Check the behavior of XATTR_CREATE.
    //

    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.test0", NULL, 0));
    LxtCheckEqual(Size, 10, "%d");
    LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], "user.test0", DynamicBuffer, 15, XATTR_CREATE), EEXIST);

    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.test0", NULL, 0));
    LxtCheckEqual(Size, 10, "%d");
    LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], "user.new", DynamicBuffer, 15, XATTR_CREATE));

    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.new", NULL, 0));
    LxtCheckEqual(Size, 15, "%d");
    strncpy(TestXattrs[Count].Name, "user.new", sizeof(TestXattrs[Count].Name) - 1);
    Count += 1;
    //
    // Check the behavior of XATTR_REPLACE.
    //

    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.test0", NULL, 0));
    LxtCheckEqual(Size, 10, "%d");
    LxtCheckErrnoFailure(LxtSetxattr(g_XattrPaths[0], "user.new2", DynamicBuffer, 15, XATTR_REPLACE), ENODATA);

    LxtCheckErrnoFailure(LxtGetxattr(g_XattrPaths[0], "user.new2", NULL, 0), ENODATA);

    LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], "user.new", DynamicBuffer, 10, XATTR_REPLACE));

    LxtCheckErrno(Size = GetXattr(g_XattrPaths[0], "user.new", NULL, 0));
    LxtCheckEqual(Size, 10, "%d");

    //
    // Set a zero-length extended attribute with XATTR_REPLACE.
    //
    // N.B. Plan 9 does not support this, as it treats this operation like a
    //      remove.
    //

    if (g_LxtFsInfo.FsType != LxtFsTypePlan9)
    {
        LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], "user.new", NULL, 0, XATTR_REPLACE));

        LxtCheckErrnoZeroSuccess(LxtGetxattr(g_XattrPaths[0], "user.new", NULL, 0));
        LxtCheckErrnoZeroSuccess(LxtSetxattr(g_XattrPaths[0], "user.new", "", 0, XATTR_REPLACE));

        LxtCheckErrnoZeroSuccess(LxtGetxattr(g_XattrPaths[0], "user.new", NULL, 0));
    }

    //
    // List the attributes.
    //

    TotalCount = Count;
    for (Index = 0; Index < 10; Index += 1)
    {
        LxtCheckErrno(DynamicBufferSize = LxtListxattr(g_XattrPaths[0], 0, NULL));
        LxtLogInfo("listxattr returned %d", DynamicBufferSize);
        DynamicBuffer = realloc(DynamicBuffer, DynamicBufferSize);
        if (DynamicBuffer == NULL)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("realloc(%p %d) failed", DynamicBuffer, DynamicBufferSize);
            goto ErrorExit;
        }

        //
        // Ensure that the number of extended attributes returned matches the number created.
        //

        DynamicBufferSize = LxtListxattr(g_XattrPaths[0], DynamicBuffer, DynamicBufferSize);
        if (DynamicBufferSize > 0)
        {
            break;
        }
        else if (errno != ERANGE)
        {
            LxtLogError("listxattr returned %d", errno);
            goto ErrorExit;
        }

        //
        // Sleep before retrying.
        //

        sleep(1);
    }

    if (DynamicBufferSize < 0)
    {
        LxtLogError("listxattr returned %d", errno);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    for (Index = 0; Index < DynamicBufferSize; Index += strlen(&DynamicBuffer[Index]) + 1)
    {
        for (NameIndex = 0; NameIndex < TotalCount; NameIndex += 1)
        {
            if (strcmp(TestXattrs[NameIndex].Name, &DynamicBuffer[Index]) == 0)
            {
                if (TestXattrs[NameIndex].Found != false)
                {
                    LxtLogError("Duplicate attribute: %s", &DynamicBuffer[Index]);
                    Result = LXT_RESULT_FAILURE;
                    goto ErrorExit;
                }

                TestXattrs[NameIndex].Found = true;
                break;
            }
        }

        if (NameIndex == TotalCount)
        {
            LxtLogError("Unknown attribute in listing: %s", &DynamicBuffer[Index]);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }

        LxtRemovexattr(g_XattrPaths[0], &DynamicBuffer[Index]);
        Count -= 1;
    }

    for (NameIndex = 0; NameIndex < TotalCount; NameIndex += 1)
    {
        if (TestXattrs[NameIndex].Found == false)
        {
            LxtLogError("Attribute missing from listing: %s", TestXattrs[NameIndex].Name);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    LxtCheckEqual(Count, 0, "%d");
    LxtCheckEqual(LxtListxattr(g_XattrPaths[0], DynamicBuffer, DynamicBufferSize), 0, "%d");

    //
    // Ensure that two extended attributes with the same name but different
    // cases can be created.
    //
    // N.B. DrvFs, WslFs and Plan 9 do not support this.
    //

    if (g_LxtFsInfo.Flags.DrvFsBehavior == 0)
    {
        LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.foo", NULL, 0, 0));
        LxtCheckErrno(LxtSetxattr(g_XattrPaths[0], "user.FOO", NULL, 0, 0));
        LxtCheckErrno(Result = LxtListxattr(g_XattrPaths[0], Buffer, sizeof(Buffer)));
        Count = 0;
        for (Index = 0; Index < Result; Index += strlen(&Buffer[Index]) + 1)
        {
            LxtLogInfo("%s", &Buffer[Index]);
            Count += 1;
        }

        LxtCheckEqual(Count, 2, "%d");
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    XattrTestDeletePaths(Fds);
    if (DynamicBuffer != NULL)
    {
        free(DynamicBuffer);
    }

    return (int)Result;
}

int XattrAccessTest(PLXT_ARGS Args)

/*++
--*/

{

    // Example system.posix_acl_access structure created by "setfacl -m u:root:r"
    char AclAccess[] = {2, 0, 0,    0,    1,    0,    6,  0, 0xff, 0xff, 0xff, 0xff, 2,    0,    4,  0, 0, 0, 0,  0,  4,  0,
                        4, 0, 0xff, 0xff, 0xff, 0xff, 16, 0, 4,    0,    0xff, 0xff, 0xff, 0xff, 32, 0, 4, 0, -1, -1, -1, -1};

    // Example security.capability structure created by "setcap cap_net_raw+ep"
    char Capability[] = {1, 0, 0, 2, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    char Buffer[1024];
    struct __user_cap_data_struct CapData[2];
    struct __user_cap_header_struct CapHeader;
    int ChildPid;
    int Fd;
    ssize_t Result;

    ChildPid = -1;
    Fd = -1;
    LxtCheckErrno(Fd = creat(LXT_XATTR_ACCESS_FILE_PATH, 0777));

    /*

    //
    // TODO: Enable this test variation once extended attributes in the system
    // namespace are supported.
    //

    //
    // Set the system.posix_acl_access EA and validate access to get, set, and list.
    //

    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", AclAccess, sizeof(AclAccess), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", Buffer, sizeof(Buffer)));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0000));

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0) {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(LXT_XATTR_GID));
        LxtCheckErrno(setuid(LXT_XATTR_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_FOWNER)].permitted |= CAP_TO_MASK(CAP_FOWNER);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("system.posix_acl_access"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", Buffer, sizeof(Buffer)));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", AclAccess, sizeof(AclAccess), 0));

        //
        // Drop the CAP_FOWNER capability and attempt again.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("system.posix_acl_access"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", Buffer, sizeof(Buffer)));
        LxtCheckErrnoFailure(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", AclAccess, sizeof(AclAccess), 0),
    EPERM); Result = LXT_RESULT_SUCCESS; goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0000));

    //
    // Set the system.posix_acl_access EA and ensure it is able to be queried.
    //

    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", AclAccess, sizeof(AclAccess), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", Buffer, sizeof(Buffer)));
    LxtCheckErrno(chown(LXT_XATTR_ACCESS_FILE_PATH, LXT_XATTR_UID, LXT_XATTR_GID));

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0) {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(LXT_XATTR_GID));
        LxtCheckErrno(setuid(LXT_XATTR_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_DAC_OVERRIDE)].permitted |= CAP_TO_MASK(CAP_DAC_OVERRIDE);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("system.posix_acl_access"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", Buffer, sizeof(Buffer)));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", AclAccess, sizeof(AclAccess), 0));

        //
        // Drop the CAP_DAC_OVERRIDE capability and attempt again.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("system.posix_acl_access"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", Buffer, sizeof(Buffer)));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access", AclAccess, sizeof(AclAccess), 0));
        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtRemovexattr(LXT_XATTR_ACCESS_FILE_PATH, "system.posix_acl_access"));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0777));

    */

    //
    // Set the security.capability EA and validate access to get, set, and list.
    //

    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Capability, sizeof(Capability), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Buffer, sizeof(Buffer)));
    LxtCheckEqual(Result, sizeof(Capability), "%Iu");
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0000));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(LXT_XATTR_GID));
        LxtCheckErrno(setuid(LXT_XATTR_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SETFCAP)].permitted |= CAP_TO_MASK(CAP_SETFCAP);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("security.capability"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Buffer, sizeof(Buffer)));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Capability, sizeof(Capability), 0));

        //
        // Drop the CAP_SETFCAP capability and attempt again.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("security.capability"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Buffer, sizeof(Buffer)));
        LxtCheckEqual(Result, sizeof(Capability), "%Iu");
        LxtCheckErrnoFailure(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Capability, sizeof(Capability), 0), EPERM);
        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Validate that the security.capability EA is removed when the file changes owners.
    //

    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Capability, sizeof(Capability), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Buffer, sizeof(Buffer)));
    LxtCheckErrno(chown(LXT_XATTR_ACCESS_FILE_PATH, 0, 0));
    LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
    LxtCheckEqual(Result, 0, "%Iu");
    LxtCheckErrnoFailure(LxtGetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Buffer, sizeof(Buffer)), ENODATA);
    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Capability, sizeof(Capability), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Buffer, sizeof(Buffer)));
    LxtCheckEqual(Result, sizeof(Capability), "%Iu");
    LxtCheckErrno(LxtRemovexattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability"));
    LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
    LxtCheckEqual(Result, 0, "%Iu");
    LxtCheckErrnoFailure(LxtGetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.capability", Buffer, sizeof(Buffer)), ENODATA);
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0777));

    //
    // Set the security.foo EA and validate access to get, set, and list.
    //

    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Capability, sizeof(Capability), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Buffer, sizeof(Buffer)));
    LxtCheckEqual(Result, sizeof(Capability), "%Iu");
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0000));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setgid(LXT_XATTR_GID));
        LxtCheckErrno(setuid(LXT_XATTR_UID));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SYS_ADMIN)].permitted |= CAP_TO_MASK(CAP_SYS_ADMIN);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("security.foo"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Buffer, sizeof(Buffer)));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Capability, sizeof(Capability), 0));

        //
        // Drop the CAP_SYS_ADMIN capability and attempt again.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("security.foo"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Buffer, sizeof(Buffer)));
        LxtCheckEqual(Result, sizeof(Capability), "%Iu");
        LxtCheckErrnoFailure(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Capability, sizeof(Capability), 0), EPERM);
        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // Validate that the security.foo EA is not removed when the file changes owners.
    //

    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Capability, sizeof(Capability), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Buffer, sizeof(Buffer)));
    LxtCheckErrno(chown(LXT_XATTR_ACCESS_FILE_PATH, 0, 0));
    LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
    LxtCheckEqual(Result, sizeof("security.foo"), "%Iu");
    LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Buffer, sizeof(Buffer)));
    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Capability, sizeof(Capability), 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Buffer, sizeof(Buffer)));
    LxtCheckEqual(Result, sizeof(Capability), "%Iu");
    LxtCheckErrno(LxtRemovexattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo"));
    LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
    LxtCheckEqual(Result, 0, "%Iu");
    LxtCheckErrnoFailure(LxtGetxattr(LXT_XATTR_ACCESS_FILE_PATH, "security.foo", Buffer, sizeof(Buffer)), ENODATA);
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0777));

    //
    // Set the user.foo EA and ensure it is able to be queried.
    //

    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", LXT_XATTR_TEST_VALUE, LXT_XATTR_TEST_LENGTH, 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", NULL, 0));
    LxtCheckErrno(chown(LXT_XATTR_ACCESS_FILE_PATH, LXT_XATTR_UID, LXT_XATTR_GID));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0700));

    //
    // Fork and change the child to a UID that does not own the file.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(setuid(LXT_XATTR_UID));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("user.foo"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", NULL, 0));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", LXT_XATTR_TEST_VALUE, LXT_XATTR_TEST_LENGTH, 0));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0070));

    //
    // Fork and change the child to a UID that does not own the file.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // First try with the CAP_DAC_OVERRIDE capability.
        //

        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        LxtCheckErrno(setuid(LXT_XATTR_UID + 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_DAC_OVERRIDE)].permitted |= CAP_TO_MASK(CAP_DAC_OVERRIDE);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("user.foo"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", NULL, 0));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", LXT_XATTR_TEST_VALUE, LXT_XATTR_TEST_LENGTH, 0));

        //
        // Drop the CAP_DAC_OVERRIDE capability and attempt again.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("user.foo"), "%Iu");
        LxtCheckErrnoFailure(LxtGetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", NULL, 0), EACCES);
        LxtCheckErrnoFailure(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", LXT_XATTR_TEST_VALUE, LXT_XATTR_TEST_LENGTH, 0), EACCES);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0007));

    //
    // Fork and change the child to a UID that does not own the file with the
    // other bits set.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(setuid(LXT_XATTR_UID + 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("user.foo"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", NULL, 0));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", LXT_XATTR_TEST_VALUE, LXT_XATTR_TEST_LENGTH, 0));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0002));

    //
    // Fork and change the child to a UID that does not own the file with the
    // other bits set.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(setuid(LXT_XATTR_UID + 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("user.foo"), "%Iu");
        LxtCheckErrnoFailure(LxtGetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", NULL, 0), EACCES);
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", LXT_XATTR_TEST_VALUE, LXT_XATTR_TEST_LENGTH, 0));
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0004));

    //
    // Fork and change the child to a UID that does not own the file with the
    // other bits set.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(setuid(LXT_XATTR_UID + 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("user.foo"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", NULL, 0));
        LxtCheckErrnoFailure(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo", LXT_XATTR_TEST_VALUE, LXT_XATTR_TEST_LENGTH, 0), EACCES);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
    LxtCheckErrno(LxtRemovexattr(LXT_XATTR_ACCESS_FILE_PATH, "user.foo"));
    LxtCheckErrno(chown(LXT_XATTR_ACCESS_FILE_PATH, 0, 0));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0777));

    //
    // Test the trusted namespace, the caller requires the CAP_SYS_ADMIN
    // capability to read to write EA's in the trusted namespace.
    //

    Buffer[0] = 'y';
    LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "trusted.overlay.opaque", Buffer, 1, 0));
    LxtCheckErrno(Result = GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "trusted.overlay.opaque", Buffer, sizeof(Buffer)));
    LxtCheckErrno(chmod(LXT_XATTR_ACCESS_FILE_PATH, 0000));

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(prctl(PR_SET_KEEPCAPS, 1));
        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        CapData[CAP_TO_INDEX(CAP_SYS_ADMIN)].permitted |= CAP_TO_MASK(CAP_SYS_ADMIN);
        CapData[0].effective = CapData[0].permitted;
        CapData[1].effective = CapData[1].permitted;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        LxtCheckEqual(Result, sizeof("trusted.overlay.opaque"), "%Iu");
        LxtCheckErrno(GetXattr(LXT_XATTR_ACCESS_FILE_PATH, "trusted.overlay.opaque", NULL, 0));
        LxtCheckErrno(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "trusted.overlay.opaque", Buffer, 1, 0));

        //
        // Drop the CAP_SYS_ADMIN capability and attempt again.
        //
        // N.B. Unlike other namespaces, names in the trusted namespace will
        //      not be returned if the caller does not have the correct permission.
        //      This is file system specific, and Plan 9 does not do this.
        //

        memset(&CapData, 0, sizeof(CapData));
        memset(&CapHeader, 0, sizeof(CapHeader));
        CapHeader.version = _LINUX_CAPABILITY_VERSION_3;
        LxtCheckErrno(LxtCapSet(&CapHeader, CapData));
        LxtCheckErrno(Result = LxtListxattr(LXT_XATTR_ACCESS_FILE_PATH, 0, NULL));
        if ((g_LxtFsInfo.FsType == LxtFsTypePlan9) || (g_LxtFsInfo.FsType == LxtFsTypeVirtioFs))
        {
            LxtCheckEqual(Result, sizeof("trusted.overlay.opaque"), "%Iu");
        }
        else
        {
            LxtCheckEqual(Result, 0, "%Iu");
        }

        LxtCheckErrnoFailure(LxtGetxattr(LXT_XATTR_ACCESS_FILE_PATH, "trusted.overlay.opaque", NULL, 0), ENODATA);
        LxtCheckErrnoFailure(LxtSetxattr(LXT_XATTR_ACCESS_FILE_PATH, "trusted.overlay.opaque", Buffer, 1, 0), EPERM);
        goto ErrorExit;
    }

    //
    // Wait for the child to exit.
    //

    LxtCheckErrno(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != 0)
    {
        LxtClose(Fd);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    unlink(LXT_XATTR_ACCESS_FILE_PATH);
    return (int)Result;
}
