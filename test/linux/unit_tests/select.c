/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    select.c

Abstract:

    This file is the select system call test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

#if defined(__aarch64__)

//
// ARM64 glibc converts select to pselect.
//

#define __ARCH_WANT_SYSCALL_DEPRECATED
#include <linux/unistd.h>
#define SYS_select __NR_select
#endif

#define LxtSelect(_nfds, _readfds, _writefds, _exceptfds, _timeout) \
    (syscall(SYS_select, _nfds, _readfds, _writefds, _exceptfds, _timeout))

#define LXT_NAME "Select"
#define LXT_SELECT_TEST_FILE "/data/test/select_test.bin"
#define LXT_SHORT_TIMEOUT 1

LXT_VARIATION_HANDLER SelectFdBufferSize;
LXT_VARIATION_HANDLER SelectMaxNfds;

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {{"FD buffer sizes", SelectFdBufferSize}, {"Max nfds", SelectMaxNfds}};

int SelectTestEntry(int Argc, char* Argv[])

/*++
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

int SelectFdBufferSize(PLXT_ARGS Args)
{

    unsigned char* Address;
    int Fds[sizeof(unsigned long) * 8 * 2];
    int Index;
    void* MapResult;
    int Result;
    fd_set* ReadSet;
    unsigned char* ReadSetBuffer;
    int ReadSetCount;
    struct timeval Timeout;

    Address = NULL;
    memset(Fds, -1, sizeof(Fds));
    Result = LXT_RESULT_FAILURE;

    //
    // Open files that will be used for select.
    //

    for (Index = 0; Index < LXT_COUNT_OF(Fds); ++Index)
    {
        LxtCheckErrno(Fds[Index] = open(LXT_SELECT_TEST_FILE, O_RDWR | O_CREAT, S_IRWXU));
    }

    //
    // Create a read\write page followed by a no access page. The readset buffer
    // will be adjusted so it is just before the no access page.
    //

    LxtCheckMapErrno(Address = mmap(NULL, PAGE_SIZE * 2, (PROT_READ | PROT_WRITE), (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0));
    LxtCheckErrno(mprotect(Address + PAGE_SIZE, PAGE_SIZE, PROT_NONE));

    //
    // A ReadSetCount of 0 should not touch the buffer.
    //

    ReadSetCount = 0;
    ReadSetBuffer = (Address + PAGE_SIZE) - sizeof(unsigned char);
    memset(ReadSetBuffer, -1, sizeof(unsigned char));
    ReadSet = (fd_set*)ReadSetBuffer;
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(LxtSelect(ReadSetCount, ReadSet, NULL, NULL, &Timeout));
    LxtCheckEqual(*ReadSetBuffer, (unsigned char)-1, "%c");

    //
    // Test select with different sized buffers that have all of the bits set
    // and a ReadSetCount of 1. The expectation is that the write will fail if
    // the buffer is less than an unsigned long. If larger, the values will be
    // zeroed out to an unsigned long but not more.
    //

    ReadSetCount = 1;
    ReadSetBuffer = (Address + PAGE_SIZE) - sizeof(unsigned char);
    memset(ReadSetBuffer, -1, sizeof(unsigned char));
    ReadSet = (fd_set*)ReadSetBuffer;
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrnoFailure(LxtSelect(ReadSetCount, ReadSet, NULL, NULL, &Timeout), EFAULT);
    LxtCheckEqual(*ReadSetBuffer, (unsigned char)-1, "%c");

    if (sizeof(unsigned int) != sizeof(unsigned long))
    {
        ReadSetBuffer = (Address + PAGE_SIZE) - sizeof(unsigned int);
        memset(ReadSetBuffer, -1, sizeof(unsigned int));
        ReadSet = (fd_set*)ReadSetBuffer;
        memset(&Timeout, 0, sizeof(Timeout));
        LxtCheckErrnoFailure(LxtSelect(ReadSetCount, ReadSet, NULL, NULL, &Timeout), EFAULT);
        LxtCheckEqual(*(unsigned int*)ReadSetBuffer, (unsigned int)-1, "%d");
    }

    ReadSetBuffer = (Address + PAGE_SIZE) - sizeof(unsigned long);
    memset(ReadSetBuffer, -1, sizeof(unsigned long));
    ReadSet = (fd_set*)ReadSetBuffer;
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(LxtSelect(ReadSetCount, ReadSet, NULL, NULL, &Timeout));
    LxtCheckEqual(*((unsigned long*)ReadSetBuffer), 0, "%ld");

    ReadSetBuffer = (Address + PAGE_SIZE) - (sizeof(unsigned long) * 2);
    memset(ReadSetBuffer, -1, sizeof(unsigned long) * 2);
    ReadSet = (fd_set*)ReadSetBuffer;
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(LxtSelect(ReadSetCount, ReadSet, NULL, NULL, &Timeout));
    LxtCheckEqual(*((unsigned long*)ReadSetBuffer), 0, "%ld");
    LxtCheckEqual(*(((unsigned long*)ReadSetBuffer + 1)), -1, "%ld");

    //
    // Test with a ReadSetCount of exactly the number of bits in an unsigned
    // long.
    //

    ReadSetCount = sizeof(unsigned long) * 8;
    ReadSetBuffer = (Address + PAGE_SIZE) - (sizeof(unsigned long) * 2);
    memset(ReadSetBuffer, -1, sizeof(unsigned long) * 2);
    ReadSet = (fd_set*)ReadSetBuffer;
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(LxtSelect(ReadSetCount, ReadSet, NULL, NULL, &Timeout));
    LxtCheckNotEqual(*((unsigned long*)ReadSetBuffer), -1, "%ld");
    LxtCheckEqual(*(((unsigned long*)ReadSetBuffer + 1)), -1, "%ld");

    //
    // And again with one larger.
    //

    ReadSetCount = sizeof(unsigned long) * 8 + 1;
    ReadSetBuffer = (Address + PAGE_SIZE) - (sizeof(unsigned long) * 2);
    memset(ReadSetBuffer, -1, sizeof(unsigned long) * 2);
    ReadSet = (fd_set*)ReadSetBuffer;
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(LxtSelect(ReadSetCount, ReadSet, NULL, NULL, &Timeout));
    LxtCheckNotEqual(*((unsigned long*)ReadSetBuffer), -1, "%ld");
    LxtCheckEqual(*(((unsigned long*)ReadSetBuffer + 1)), 1, "%ld");

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    for (Index = 0; Index < LXT_COUNT_OF(Fds); ++Index)
    {
        if (Fds[Index] != -1)
        {
            LxtClose(Fds[Index]);
        }
    }

    if (Address != NULL)
    {
        munmap(Address, PAGE_SIZE * 2);
    }

    return Result;
}

int SelectMaxNfds(PLXT_ARGS Args)
{

    int Result;
    int Fd;
    int NumFd;
    fd_set ReadSet;
    struct timeval Timeout;
    struct rlimit MaxFds;

    //
    // Initialize locals.
    //

    Fd = -1;
    Timeout.tv_sec = 5;
    Timeout.tv_usec = 0;

    //
    // Open a file that will be used for select.
    //

    LxtCheckErrno(Fd = open(LXT_SELECT_TEST_FILE, (O_RDWR | O_CREAT), S_IRWXU));

    //
    // Create the select set and set the FD in it.
    //

    FD_ZERO(&ReadSet);
    FD_SET(Fd, &ReadSet);

    //
    // -ve values for 'nfds' should return EINVAL.
    //

    LxtCheckErrnoFailure(select(-1, &ReadSet, NULL, NULL, &Timeout), EINVAL);

    LxtLogInfo("Waiting on select to succeed..");

    //
    // Set 'nfds' to > FD_SETSIZE. Kernel seems to ignore anything above the
    // current ulimit(RLIMIT_NOFILE).
    //
    // N.B The value chosen for 'nfds' is > nr_open (which is the upper limit
    //     for RLIMIT_NOFILE and by default set to 1048576). As per the man
    //     page, EINVAL is returned if nfds exceeds the RLIMIT_NOFILE resource
    //     limit, but that doesn't seem to be case.
    //

    LxtCheckErrno(NumFd = select(1048576 + 100, &ReadSet, NULL, NULL, &Timeout));

    LxtCheckEqual(NumFd, 1, "%d");
    if (FD_ISSET(Fd, &ReadSet) == 0)
    {
        LxtLogError(
            "Select was satisfied but file descriptor is not set "
            "for read!");

        Result = -1;
        goto ErrorExit;
    }

    //
    // Provide a bad file descriptor to select. As per the man page kernel
    // ignores any FD > maximum FD currently opened by the process. But,
    // in testing it seems like it does perform that check.
    //
    // N.B Below assumes that 200 > #FD's opened by the process.
    //

    FD_ZERO(&ReadSet);
    FD_SET(200, &ReadSet);

    //
    //  Kernel ignores anything above FD_SETSIZE.
    //

    LxtCheckErrnoFailure(select(201, &ReadSet, NULL, NULL, &Timeout), EBADF);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        close(Fd);
    }

    return Result;
}