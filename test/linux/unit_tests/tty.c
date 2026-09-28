/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    tty.c

Abstract:

    This file contains unit tests for the /dev/tty0

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <termios.h>
#include <sys/ioctl.h>

#define LXT_NAME "tty"

#define LXT_MANUAL_OUTPUT "ttyOutput.txt"

#define LXT_NON_DEFAULT_MODE (S_IFCHR | 0111)

#define LXT_NON_DEFAULT_ID 255

LXT_VARIATION_HANDLER TestDevTty0Ioctl;

LXT_VARIATION_HANDLER TestDevTtyIoctl;

LXT_VARIATION_HANDLER TestDevTtyStat;

LXT_VARIATION_HANDLER TestDevTtyOpen;

LXT_VARIATION_HANDLER TestDevTtySecurity;

//
// TODO: Enable TestDevTty0Ioctl
//

static const LXT_VARIATION g_LxtVariations[] = {
    // {"Test the implementation of the ioctl(/dev/tty0)", TestDevTty0Ioctl},
    {"tty stat", TestDevTtyStat},
    {"tty open", TestDevTtyOpen},
    {"tty security", TestDevTtySecurity},
    {"tty ioctl", TestDevTtyIoctl}};

int TtyTestEntry(int argc, char* argv[])
{
    LXT_ARGS Args;
    int Result;

    //
    // Started in a unit test mode.
    //

    LxtCheckResult(LxtInitialize(argc, argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int TestDevTty0Ioctl(PLXT_ARGS Args)

{

    int fd;
    int err;
    char path[] = "/dev/tty0";
    struct vt_stat vt_stat;
    int vt_index;
    struct termios termios;
    int i;

    err = -1;
    fd = -1;

    //
    // Open the target.
    //

    fd = open(path, O_RDWR);
    if (-1 == fd)
    {
        err = errno;
        LxtLogError("open('%s') failed, %d", path, err);
        goto exit;
    }

    //
    // Test the ioctl(KDSETMODE)
    //

    err = ioctl(fd, KDSETMODE, KD_TEXT);
    if (-1 == err)
    {
        err = errno;
        LxtLogError("ioctl('%s', KDSETMODE, KD_TEXT) failed, %d", path, err);
        goto exit;
    }

    LxtLogInfo("ioctl('%s', KDSETMODE) -> %d ", path, err);

    //
    // Test the ioctl(VT_GETSTATE)
    //

    err = ioctl(fd, VT_GETSTATE, &vt_stat);
    if (-1 == err)
    {
        err = errno;
        LxtLogError("ioctl('%s', VT_GETSTATE) failed, %d", path, err);
        goto exit;
    }

    LxtLogInfo("ioctl('%s', VT_GETSTATE) -> %d ", path, err);
    LxtLogInfo("    vt_stat.v_active = %d", vt_stat.v_active);
    LxtLogInfo("    vt_stat.v_signal = %d", vt_stat.v_signal);
    LxtLogInfo("    vt_stat.v_state  = %d", vt_stat.v_state);

    //
    // Test the activation of the VT#7
    //

    vt_index = 7;

    err = ioctl(fd, VT_ACTIVATE, vt_index);
    if (-1 == err)
    {
        err = errno;
        LxtLogError("ioctl('%s', VT_ACTIVATE, %d) failed, %d", path, vt_index, err);
        goto exit;
    }

    LxtLogInfo("ioctl('%s', VT_ACTIVATE, %d) -> %d ", path, vt_index, err);

    err = ioctl(fd, VT_WAITACTIVE, vt_index);
    if (-1 == err)
    {
        err = errno;
        LxtLogError("ioctl('%s', VT_WAITACTIVE, %d) failed, %d", path, vt_index, err);
        goto exit;
    }

    LxtLogInfo("ioctl('%s', VT_WAITACTIVE, %d) -> %d ", path, vt_index, err);

    //
    // Get/set port settings.
    //

#if !defined(__amd64__)

    err = ioctl(fd, TCGETS, &termios);
    if (-1 == err)
    {
        err = errno;
        LxtLogError("ioctl('%s', TCGETS) failed, %d", path, err);
        goto exit;
    }

    LxtLogInfo("ioctl('%s', TCGETS) -> %d ", path, err);
    LxtLogInfo("    termios.c_iflag  = %d", termios.c_iflag);
    LxtLogInfo("    termios.c_oflag  = %d", termios.c_oflag);
    LxtLogInfo("    termios.c_cflag  = %d", termios.c_cflag);
    LxtLogInfo("    termios.c_lflag  = %d", termios.c_lflag);
    LxtLogInfo("    termios.c_line  = %d", termios.c_line);
    for (i = 0; i < NCCS; i++)
    {
        LxtLogInfo("    termios.c_cc[%d] = %d", i, termios.c_cc[i]);
    }

#endif

    //
    // Test the ioctl(VT_GETSTATE) after all preparation completed.
    //

    err = ioctl(fd, VT_GETSTATE, &vt_stat);
    if (-1 == err)
    {
        err = errno;
        LxtLogError("ioctl('%s', VT_GETSTATE) failed, %d", path, err);
        goto exit;
    }

    LxtLogInfo("ioctl('%s', VT_GETSTATE) -> %d ", path, err);
    LxtLogInfo("    vt_stat.v_active = %d", vt_stat.v_active);
    LxtLogInfo("    vt_stat.v_signal = %d", vt_stat.v_signal);
    LxtLogInfo("    vt_stat.v_state  = %d", vt_stat.v_state);

    //
    // Done.
    // Close the test device handle.
    //

    close(fd);
    fd = -1;

    err = 0;

exit:
    if (-1 != fd)
    {
        close(fd);
    }

    return err;
}

int TestDevTtyIoctl(PLXT_ARGS Args)

{
    int Mode = 0;
    int Result;

    LxtCheckErrno(ioctl(0, KDGKBTYPE, &Mode));
    LxtCheckEqual(Mode, KB_101, "%d");
    LxtCheckErrno(ioctl(0, KDGKBMODE, &Mode));
    LxtCheckEqual(Mode, K_UNICODE, "%d");
    LxtCheckErrno(ioctl(0, KDSKBMODE, Mode));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int TestDevTtyStat(PLXT_ARGS Args)

{

    int Index;
    int Result;
    struct stat StatFd;
    struct stat StatFile;
    char TtyName[32];

    //
    // stat the file through the fd and tty name result.
    //

    for (Index = 0; Index < 3; ++Index)
    {
        LxtCheckErrno(ttyname_r(Index, TtyName, sizeof(TtyName)));
        LxtLogInfo("Name %d: %s", Index, TtyName);
        LxtCheckErrno(fstat(Index, &StatFd));
        LxtCheckErrno(stat(TtyName, &StatFile));
        LxtCheckEqual(StatFd.st_dev, StatFile.st_dev, "%d");
        LxtCheckNotEqual(StatFd.st_dev, 0, "%d");
        LxtCheckEqual(StatFd.st_ino, StatFile.st_ino, "%d");
        LxtCheckNotEqual(StatFd.st_ino, 0, "%d");
        LxtCheckEqual(StatFd.st_mode, StatFile.st_mode, "%d");
        LxtCheckNotEqual(StatFd.st_mode, 0, "%d");
        LxtCheckEqual(StatFd.st_nlink, StatFile.st_nlink, "%d");
        LxtCheckEqual(StatFd.st_nlink, 1, "%d");
        LxtCheckEqual(StatFd.st_uid, StatFile.st_uid, "%d");
        LxtCheckEqual(StatFd.st_uid, 0, "%d");
        LxtCheckEqual(StatFd.st_gid, StatFile.st_gid, "%d");
        LxtCheckEqual(StatFd.st_gid, 5, "%d");
        LxtCheckEqual(StatFd.st_rdev, StatFile.st_rdev, "%d");
        LxtCheckNotEqual(StatFd.st_rdev, 0, "%d");
        LxtCheckEqual(StatFd.st_size, StatFile.st_size, "%d");
        LxtCheckEqual(StatFd.st_size, 0, "%d");
        LxtCheckEqual(StatFd.st_blocks, StatFile.st_blocks, "%d");
        LxtCheckEqual(StatFd.st_blocks, 0, "%d");
    }

    Result = 0;

ErrorExit:
    return Result;
}

int TestDevTtyOpen(PLXT_ARGS Args)

{

    ssize_t BytesRead;
    int Index;
    char LinkName[32];
    char LinkTarget[32];
    int Result;
    struct stat StatFd;
    struct stat StatFile;
    int TtyFd;
    char TtyName[32];
    char TtyNameFd[32];

    TtyFd = -1;

    //
    // Check that the current tty can be opened by name and is linked
    // appropriately in procfs.
    //

    for (Index = 0; Index < 3; ++Index)
    {
        LxtCheckErrno(ttyname_r(Index, TtyName, sizeof(TtyName)));
        LxtCheckErrno(TtyFd = open(TtyName, O_RDWR));
        LxtCheckErrno(ttyname_r(TtyFd, TtyNameFd, sizeof(TtyNameFd)));
        LxtCheckStringEqual(TtyName, TtyNameFd);

        snprintf(LinkName, sizeof(LinkName), "/proc/self/fd/%d", Index);
        LxtCheckErrno(BytesRead = readlink(LinkName, LinkTarget, sizeof(LinkTarget) - 1));
        LinkTarget[BytesRead] = 0;
        LxtCheckStringEqual(TtyName, LinkTarget);

        snprintf(LinkName, sizeof(LinkName), "/proc/self/fd/%d", TtyFd);
        LxtCheckErrno(BytesRead = readlink(LinkName, LinkTarget, sizeof(LinkTarget) - 1));
        LinkTarget[BytesRead] = 0;
        LxtCheckStringEqual(TtyName, LinkTarget);

        LxtCheckErrno(fstat(TtyFd, &StatFd));
        LxtCheckErrno(fstat(Index, &StatFile));
        LxtCheckEqual(StatFd.st_ino, StatFile.st_ino, "%d");
        LxtCheckEqual(StatFd.st_rdev, StatFile.st_rdev, "%d");
        LxtClose(TtyFd);
        TtyFd = -1;
    }

    //
    // Check that /dev/tty0 fails to open, this behavior differs from native
    // Linux.
    //

    LxtCheckErrnoFailure(TtyFd = open("/dev/tty0", O_RDWR), EIO);
    Result = 0;

ErrorExit:
    if (TtyFd != -1)
    {
        LxtClose(TtyFd);
    }

    return Result;
}

int TestDevTtySecurity(PLXT_ARGS Args)

{

    bool ResetSecurity;
    int Result;
    struct stat Stat;
    struct stat StatOriginal;
    char TtyName[32];

    ResetSecurity = false;
    LxtCheckErrno(fstat(0, &StatOriginal));
    ResetSecurity = true;
    LxtCheckErrno(ttyname_r(0, TtyName, sizeof(TtyName)));

    //
    // Check that the uid, gid, and mode can be changed on the name or fd and
    // are reflected into the stat on the fd and name
    //

    LxtCheckErrno(chmod(TtyName, LXT_NON_DEFAULT_MODE));
    LxtCheckErrno(chown(TtyName, LXT_NON_DEFAULT_ID, LXT_NON_DEFAULT_ID));
    LxtCheckErrno(fstat(0, &Stat));
    LxtCheckEqual(Stat.st_mode, LXT_NON_DEFAULT_MODE, "%d");
    LxtCheckEqual(Stat.st_uid, LXT_NON_DEFAULT_ID, "%d");
    LxtCheckEqual(Stat.st_gid, LXT_NON_DEFAULT_ID, "%d");

    LxtCheckErrno(fchmod(0, StatOriginal.st_mode));
    LxtCheckErrno(fchown(0, StatOriginal.st_uid, StatOriginal.st_gid));
    ResetSecurity = false;
    LxtCheckErrno(stat(TtyName, &Stat));
    LxtCheckEqual(Stat.st_mode, StatOriginal.st_mode, "%d");
    LxtCheckEqual(Stat.st_uid, StatOriginal.st_uid, "%d");
    LxtCheckEqual(Stat.st_gid, StatOriginal.st_gid, "%d");

    Result = 0;

ErrorExit:
    if (ResetSecurity != false)
    {
        fchmod(0, StatOriginal.st_mode);
        fchown(0, StatOriginal.st_uid, StatOriginal.st_gid);
    }

    return Result;
}
