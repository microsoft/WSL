/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslpath.c

Abstract:

    This file contains tests for the wslpath binary.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>

#define LXT_NAME "wslpath"

#define WSLPATH_ESCAPE_NAME "wslpath_foo\\:bar"
#define WSLPATH_ESCAPE_NAME_ESCAPED "wslpath_foo\uf05c\uf03abar"
#define WSLPATH_ESCAPE_DIR "/mnt/c/" WSLPATH_ESCAPE_NAME
#define WSLPATH_ESCAPE_DIR_ESCAPED "/mnt/c/" WSLPATH_ESCAPE_NAME_ESCAPED
#define WSLPATH_ESCAPE_DIR_WIN "C:\\" WSLPATH_ESCAPE_NAME_ESCAPED
#define WSLPATH_SYMLINK_TEST_DIR "/mnt/c/symlink_test_dir"
#define WSLPATH_SYMLINK_TEST_TARGET WSLPATH_SYMLINK_TEST_DIR "/target"
#define WSLPATH_SYMLINK_TEST_LINK WSLPATH_SYMLINK_TEST_DIR "/link"
#define WSLPATH_SYMLINK_TEST_DIR_WIN "C:\\symlink_test_dir"
#define WSLPATH_SYMLINK_TEST_TARGET_WIN WSLPATH_SYMLINK_TEST_DIR_WIN "\\target"
#define WSLPATH_SYMLINK_TEST_LINK_WIN WSLPATH_SYMLINK_TEST_DIR_WIN "\\link"
#define WSLPATH_DISTRO_PREFIX "\\\\wsl.localhost\\" LXSS_DISTRO_NAME_TEST
#define WSLPATH_DISTRO_COMPAT_PREFIX "\\\\wsl$\\" LXSS_DISTRO_NAME_TEST
#define WSLPATH_ESCAPE_LX_DIR "/data/" WSLPATH_ESCAPE_NAME
#define WSLPATH_ESCAPE_LX_DIR_WIN WSLPATH_DISTRO_PREFIX "\\data\\" WSLPATH_ESCAPE_NAME_ESCAPED
#define WSLPATH_MOUNT_POINT "/data/wslpath_mount"

LXT_VARIATION_HANDLER WslPathTestDrvFsEscaped;

LXT_VARIATION_HANDLER WslPathTestDrvFsToWinPath;

LXT_VARIATION_HANDLER WslPathTestDrvFsSymlink;

LXT_VARIATION_HANDLER WslPathTestDrvFsFromWinPath;

LXT_VARIATION_HANDLER WslPathTestInvalidMountInfo;

LXT_VARIATION_HANDLER WslPathTestLxEscaped;

LXT_VARIATION_HANDLER WslPathTestLxFromWinPath;

LXT_VARIATION_HANDLER WslPathTestLxToWinPath;

static const LXT_VARIATION g_LxtVariations[] = {
    {"WslPath - Windows to DrvFs", WslPathTestDrvFsFromWinPath},
    {"WslPath - DrvFs to Windows", WslPathTestDrvFsToWinPath},
    {"WslPath - DrvFs escaped characters", WslPathTestDrvFsEscaped},
    {"WslPath - DrvFs symlinks", WslPathTestDrvFsSymlink},
    {"WslPath - \\\\wsl.localhost to Linux", WslPathTestLxFromWinPath},
    {"WslPath - Linux to \\\\wsl.localhost", WslPathTestLxToWinPath},
    {"WslPath - \\\\wsl.localhost escaped characters", WslPathTestLxEscaped},
    {"WslPath - Invalid mountinfo line", WslPathTestInvalidMountInfo},
};

int WslPathTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the wslpath tests.

Arguments:

    Argc - Supplies the number of command line arguments.

    Argv - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int WslPathTestDrvFsEscaped(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath on DrvFs paths with escaped characters.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(mkdir(WSLPATH_ESCAPE_DIR, 0777));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_ESCAPE_DIR, WSLPATH_ESCAPE_DIR_WIN, false));

    //
    // Translating win to drvfs does not unescape, since the escaped characters
    // work on drvfs.
    //

    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_ESCAPE_DIR_WIN, WSLPATH_ESCAPE_DIR_ESCAPED, true));

ErrorExit:
    rmdir(WSLPATH_ESCAPE_DIR);
    return Result;
}

int WslPathTestDrvFsFromWinPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath on Windows paths.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtCheckWslPathTranslation("C:\\", "/mnt/c/", true));
    LxtCheckResult(LxtCheckWslPathTranslation("C:\\Foo", "/mnt/c/Foo", true));
    LxtCheckResult(LxtCheckWslPathTranslation("C:\\Foo\\", "/mnt/c/Foo/", true));
    LxtCheckResult(LxtCheckWslPathTranslation("C:\\Foo\\bar", "/mnt/c/Foo/bar", true));
    LxtCheckResult(LxtCheckWslPathTranslation("C:/Foo/bar", "/mnt/c/Foo/bar", true));
    LxtCheckResult(LxtCheckWslPathTranslation("foo", "foo", true));
    LxtCheckResult(LxtCheckWslPathTranslation("foo\\", "foo/", true));

ErrorExit:
    return Result;
}

int WslPathTestDrvFsSymlink(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath on DrvFs paths with symlinks.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrnoZeroSuccess(mkdir(WSLPATH_SYMLINK_TEST_DIR, 0777));
    LxtCheckErrnoZeroSuccess(mkdir(WSLPATH_SYMLINK_TEST_TARGET, 0777));
    LxtCheckErrnoZeroSuccess(symlink(WSLPATH_SYMLINK_TEST_TARGET, WSLPATH_SYMLINK_TEST_LINK));

    //
    // Drvfs to Windows follows links.
    //

    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_SYMLINK_TEST_LINK, WSLPATH_SYMLINK_TEST_TARGET_WIN, false));

    //
    // Windows to DrvFs is text based so does not.
    //

    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_SYMLINK_TEST_LINK_WIN, WSLPATH_SYMLINK_TEST_LINK, true));

ErrorExit:
    unlink(WSLPATH_SYMLINK_TEST_LINK);
    rmdir(WSLPATH_SYMLINK_TEST_TARGET);
    rmdir(WSLPATH_SYMLINK_TEST_DIR);

    return Result;
}

int WslPathTestDrvFsToWinPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath on DrvFs paths.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtCheckWslPathTranslation("/mnt/c", "C:\\", false));
    LxtCheckResult(LxtCheckWslPathTranslation("/mnt/c/", "C:\\", false));
    LxtCheckResult(LxtCheckWslPathTranslation("/mnt/c/Users", "C:\\Users", false));
    LxtCheckResult(LxtCheckWslPathTranslation("/mnt/c/Users/", "C:\\Users\\", false));
    LxtCheckResult(LxtCheckWslPathTranslation("/mnt/c/DOESNOTEXIST/", "C:\\DOESNOTEXIST\\", false));

ErrorExit:
    return Result;
}

int WslPathTestInvalidMountInfo(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath's handling of ill-formed mountinfo lines.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    //
    // WSL1 does not allow using an empty string as the mount source. This is
    // technically a minor bug in WSL1, but it also means this test is not
    // relevant, so skip it.
    //

    if (LxtWslVersion() == 1)
    {
        LxtLogInfo("Test skipped on WSL1.");
        Result = 0;
        goto ErrorExit;
    }

    LxtCheckErrnoZeroSuccess(mkdir(WSLPATH_MOUNT_POINT, 0777));

    //
    // Using an empty string as the mount source will cause the mountinfo file
    // to have a blank field, which neither libmount nor mountutil can parse.
    // This should however not break wslpath.
    //

    LxtCheckErrnoZeroSuccess(mount("", WSLPATH_MOUNT_POINT, "tmpfs", 0, NULL));
    LxtCheckResult(LxtCheckWslPathTranslation("/mnt/c", "C:\\", false));

ErrorExit:
    umount(WSLPATH_MOUNT_POINT);
    rmdir(WSLPATH_MOUNT_POINT);
    return Result;
}

int WslPathTestLxEscaped(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath on internal Linux paths with escaped characters.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckErrno(mkdir(WSLPATH_ESCAPE_LX_DIR, 0777));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_ESCAPE_LX_DIR, WSLPATH_ESCAPE_LX_DIR_WIN, false));

    //
    // Translating \\wsl.localhost to Linux does unescape (unlike drvfs).
    //

    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_ESCAPE_LX_DIR_WIN, WSLPATH_ESCAPE_LX_DIR, true));

ErrorExit:
    rmdir(WSLPATH_ESCAPE_LX_DIR);
    return Result;
}

int WslPathTestLxFromWinPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath on \\wsl.localhost paths.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_DISTRO_PREFIX, "/", true));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_DISTRO_PREFIX "\\", "/", true));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_DISTRO_PREFIX "\\root", "/root", true));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_DISTRO_PREFIX "\\proc\\stat", "/proc/stat", true));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_DISTRO_PREFIX "/proc/stat", "/proc/stat", true));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_DISTRO_COMPAT_PREFIX "\\proc\\stat", "/proc/stat", true));
    LxtCheckResult(LxtCheckWslPathTranslation(WSLPATH_DISTRO_COMPAT_PREFIX, "/", true));
    LxtCheckResult(LxtCheckWslPathTranslation("\\\\?\\C:\\Users", "/mnt/c/Users", true));
    LxtCheckResult(LxtCheckWslPathTranslation("\\\\?\\C:\\Users\\", "/mnt/c/Users/", true));
    LxtCheckResult(LxtCheckWslPathTranslation(".", ".", true));

ErrorExit:
    return Result;
}

int WslPathTestLxToWinPath(PLXT_ARGS Args)

/*++

Description:

    This routine tests wslpath on internal Linux paths.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtCheckWslPathTranslation("/", WSLPATH_DISTRO_PREFIX "\\", false));
    LxtCheckResult(LxtCheckWslPathTranslation("/root", WSLPATH_DISTRO_PREFIX "\\root", false));
    LxtCheckResult(LxtCheckWslPathTranslation("/proc/stat", WSLPATH_DISTRO_PREFIX "\\proc\\stat", false));
    LxtCheckResult(LxtCheckWslPathTranslation("/proc/1/", WSLPATH_DISTRO_PREFIX "\\proc\\1\\", false));

ErrorExit:
    return Result;
}
