/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    random.c

Abstract:

    This file is a test for the getrandom system call and the /dev/random and
    /dev/urandom devices.

--*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <linux/random.h>
#include "lxtcommon.h"
#include "unittests.h"

#define DEV_RANDOM "/dev/random"
#define DEV_RANDOM_MAX_BYTES (512)

#define DEV_URANDOM "/dev/urandom"
#define DEV_URANDOM_MAX_BYTES (0x1FFFFFF)

#define PROC_SYS_KERNEL_RANDOM "/proc/sys/kernel/random"
#define PROC_SYS_KERNEL_RANDOM_BOOTID PROC_SYS_KERNEL_RANDOM "/boot_id"
#define PROC_SYS_KERNEL_RANDOM_ENTROPY_AVAIL PROC_SYS_KERNEL_RANDOM "/entropy_avail"
#define PROC_SYS_KERNEL_RANDOM_POOLSIZE PROC_SYS_KERNEL_RANDOM "/poolsize"
#define PROC_SYS_KERNEL_RANDOM_UUID PROC_SYS_KERNEL_RANDOM "/uuid"
#define PROC_SYS_KERNEL_RANDOM_BYTES 37

#define LXT_NAME "random"

int GetrandomSyscall(PLXT_ARGS Args);

int DevRandomDevice(PLXT_ARGS Args);

int DevUrandomDevice(PLXT_ARGS Args);

int ProcfsRandom(PLXT_ARGS Args);

static const LXT_VARIATION g_LxtVariations[] = {
    {"getrandom syscall", GetrandomSyscall},
    {"/dev/random device", DevRandomDevice},
    {"/dev/urandom device", DevUrandomDevice},
    {"/proc/sys/kernel/random", ProcfsRandom}};

int RandomTestEntry(int Argc, char* Argv[])
{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return 0;
}

int GetrandomSyscall(PLXT_ARGS Args)

{

    char* Buffer;
    size_t BufferSize;
    size_t Result;

    BufferSize = DEV_URANDOM_MAX_BYTES + 1;
    Buffer = malloc(BufferSize);
    if (Buffer == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("malloc failed");
        goto ErrorExit;
    }

    //
    // Valid parameter variations.
    //

    LxtCheckResult(LxtGetrandom(NULL, 0, 0));
    LxtCheckEqual(Result, 0, "%Iu");
    LxtCheckResult(LxtGetrandom(NULL, 0, GRND_RANDOM));
    LxtCheckResult(LxtGetrandom(NULL, 0, GRND_NONBLOCK));
    LxtCheckResult(LxtGetrandom(NULL, 0, (GRND_RANDOM | GRND_NONBLOCK)));
    LxtCheckResult(LxtGetrandom(Buffer, BufferSize, 0));
    if (Result > DEV_URANDOM_MAX_BYTES)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("BytesRead %Iu greater than expected %Iu", Result, DEV_URANDOM_MAX_BYTES);

        goto ErrorExit;
    }

    LxtCheckResult(LxtGetrandom(Buffer, BufferSize, GRND_RANDOM));
    if (Result > DEV_RANDOM_MAX_BYTES)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("BytesRead %Iu greater than expected %Iu", Result, DEV_RANDOM_MAX_BYTES);

        goto ErrorExit;
    }

    //
    // Invalid parameter variations.
    //

    LxtCheckErrnoFailure(LxtGetrandom(NULL, 0, ((GRND_RANDOM | GRND_NONBLOCK) + 1)), EINVAL);
    LxtCheckErrnoFailure(LxtGetrandom(NULL, 0, -1), EINVAL);
    LxtCheckErrnoFailure(LxtGetrandom(NULL, 1, 0), EFAULT);
    LxtCheckErrnoFailure(LxtGetrandom(-1, 1, 0), EFAULT);

ErrorExit:
    if (Buffer != NULL)
    {
        free(Buffer);
    }

    return Result;
}

int DevRandomDevice(PLXT_ARGS Args)
{

    char Buffer[DEV_RANDOM_MAX_BYTES + 1];
    int BytesRead;
    int Fd;
    int Result;

    Fd = -1;

    LxtCheckResult(Fd = open(DEV_RANDOM, O_RDONLY));
    LxtCheckResult(BytesRead = read(Fd, Buffer, LXT_COUNT_OF(Buffer)));

    if (BytesRead > DEV_RANDOM_MAX_BYTES)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("BytesRead %d greater than expected %d", BytesRead, DEV_RANDOM_MAX_BYTES);

        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int DevUrandomDevice(PLXT_ARGS Args)
{

    char* Buffer;
    size_t BufferSize;
    int BytesRead;
    int Fd;
    int Result;

    BufferSize = DEV_URANDOM_MAX_BYTES + 1;
    Fd = -1;
    Buffer = malloc(BufferSize);
    if (Buffer == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("malloc failed");
        goto ErrorExit;
    }

    LxtCheckResult(Fd = open(DEV_URANDOM, O_RDONLY));
    LxtCheckResult(BytesRead = read(Fd, Buffer, BufferSize));
    if (BytesRead > DEV_URANDOM_MAX_BYTES)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("BytesRead %d greater than expected %d", BytesRead, DEV_URANDOM_MAX_BYTES);

        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Buffer != NULL)
    {
        free(Buffer);
    }

    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}

int ProcfsRandom(PLXT_ARGS Args)

{
    char Buffer[PROC_SYS_KERNEL_RANDOM_BYTES];
    int BytesRead;
    int Fd;
    int Result;

    //
    // Test /proc/sys/kernel/random/uuid.
    //

    LxtCheckResult(Fd = open(PROC_SYS_KERNEL_RANDOM_UUID, O_RDONLY));
    LxtCheckResult(BytesRead = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(PROC_SYS_KERNEL_RANDOM_BYTES, BytesRead, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Test /proc/sys/kernel/random/boot_id.
    //

    LxtCheckResult(Fd = open(PROC_SYS_KERNEL_RANDOM_BOOTID, O_RDONLY));
    LxtCheckResult(BytesRead = read(Fd, Buffer, sizeof(Buffer)));
    LxtCheckEqual(PROC_SYS_KERNEL_RANDOM_BYTES, BytesRead, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Test /proc/sys/kernel/random/entropy_avail.
    //

    LxtCheckResult(Fd = open(PROC_SYS_KERNEL_RANDOM_ENTROPY_AVAIL, O_RDONLY));
    LxtCheckResult(BytesRead = read(Fd, Buffer, (sizeof(Buffer) - 1)));
    LxtCheckEqual(5, BytesRead, "%d");
    LxtClose(Fd);
    Fd = -1;

    //
    // Test /proc/sys/kernel/random/poolsize.
    //

    LxtCheckResult(Fd = open(PROC_SYS_KERNEL_RANDOM_POOLSIZE, O_RDONLY));
    LxtCheckResult(BytesRead = read(Fd, Buffer, (sizeof(Buffer) - 1)));
    LxtCheckEqual(5, BytesRead, "%d");
    Buffer[BytesRead] = '\0';
    LxtCheckStringEqual(Buffer, "4096\n");
    LxtClose(Fd);
    Fd = -1;

ErrorExit:
    if (Fd != -1)
    {
        LxtClose(Fd);
    }

    return Result;
}
