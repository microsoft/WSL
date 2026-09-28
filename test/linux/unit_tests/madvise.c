/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    madvise.c

Abstract:

    This file is a madvise test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <string.h>
#include <sys/mman.h>
#include <sched.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define LXT_NAME "madvise"

int MadviseVariation2(PLXT_ARGS Args);

int MadviseVariation3(PLXT_ARGS Args);

int MadviseVariation4(PLXT_ARGS Args);

int MadviseVariation5(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Anonymous mapping", MadviseVariation2},
    {"Anonymous mapping with fork", MadviseVariation3},
    {"File-backed mapping", MadviseVariation4},
    {"File-backed with fork", MadviseVariation5}};

int MadviseTestEntry(int Argc, char* Argv[])

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

#define MADVISE_TEST_BUFFER_SIZE 0x5000

int MadviseRunTest(int FileId, int BufferSize, int DoFork)
{
    int Result;
    unsigned char* SharedBuffer;
    unsigned char* PrivateBuffer;
    unsigned char* Ptr;
    unsigned char* EndPtr;
    unsigned char ExpectedByte;
    int MapAnonymousFlag;
    int IsChild;

    //
    // Initialize locals.
    //

    SharedBuffer = MAP_FAILED;
    PrivateBuffer = MAP_FAILED;
    IsChild = 0;

    MapAnonymousFlag = 0;

    if (FileId == -1)
    {
        MapAnonymousFlag = MAP_ANONYMOUS;
    }

    //
    // Allocate the shared buffer.
    //

    SharedBuffer = mmap(NULL, BufferSize, PROT_READ | PROT_WRITE, MAP_SHARED | MapAnonymousFlag, FileId, 0);

    if (SharedBuffer == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Could not map shared! %d", Result);
        goto cleanup;
    }

    //
    // Allocate the private buffer.
    //

    PrivateBuffer = mmap(NULL, BufferSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MapAnonymousFlag, FileId, 0);

    if (PrivateBuffer == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Could not map private! %d", Result);
        goto cleanup;
    }

    //
    // Fill the shared buffer.
    //

    Ptr = SharedBuffer;
    EndPtr = Ptr + BufferSize;

    for (; Ptr < EndPtr; Ptr += 1)
    {
        *Ptr = 0xDE;
    }

    //
    // Make sure the private buffer sees the shared buffer.
    //

    if (FileId != -1)
    {

        Ptr = PrivateBuffer;
        EndPtr = Ptr + BufferSize;

        for (; Ptr < EndPtr; Ptr += 1)
        {
            if (*Ptr != 0xDE)
            {
                *((volatile int*)0) = 0xDEADBEEF;
            }
        }
    }

    //
    // Fill the private buffer.
    //

    Ptr = PrivateBuffer;
    EndPtr = Ptr + BufferSize;

    for (; Ptr < EndPtr; Ptr += 1)
    {
        *Ptr = 0xCA;
    }

    //
    // Protect one of the two pages that will be reverted.
    //

    Result = mprotect(PrivateBuffer + PAGE_SIZE, PAGE_SIZE, PROT_READ);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Could not set protection on buffer! %d", Result);
        goto cleanup;
    }

    //
    // Fork the process if desired.
    //

    if (DoFork)
    {

        Result = fork();

        if (Result == -1)
        {
            Result = errno;
            LxtLogError("Could not fork the child! %d", Result);
            goto cleanup;
        }

        if (Result == 0)
        {
            IsChild = 1;
        }
    }

    //
    // Revert the middle two pages back to the shared buffer.
    //

    Result = madvise((PrivateBuffer + PAGE_SIZE), (PAGE_SIZE * 2), MADV_DONTNEED);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Could not madvise on private buffer! %d", Result);
        goto cleanup;
    }

    //
    // Make sure the first page contains private data.
    //

    Ptr = PrivateBuffer;
    EndPtr = Ptr + PAGE_SIZE;

    for (; Ptr < EndPtr; Ptr += 1)
    {

        if (*Ptr != 0xCA)
        {
            LxtLogError("Private buffer not contain expected data (1), %d", *Ptr);
            Result = EFAULT;
            goto cleanup;
        }
    }

    //
    // Make sure the next two pages contain shared data or zeroes in the private
    // allocation case.
    //

    EndPtr = Ptr + (PAGE_SIZE * 2);

    for (; Ptr < EndPtr; Ptr += 1)
    {
        if (FileId == -1)
        {
            ExpectedByte = 0;
        }
        else
        {
            ExpectedByte = 0xDE;
        }

        if (*Ptr != ExpectedByte)
        {
            LxtLogError("Private buffer not contain expected data (2), %d", *Ptr);
            Result = EFAULT;
            goto cleanup;
        }
    }

    //
    // Make sure the remaining pages contain private data.
    //

    EndPtr = PrivateBuffer + BufferSize;

    for (; Ptr < EndPtr; Ptr += 1)
    {
        if (*Ptr != 0xCA)
        {
            LxtLogError("Private buffer not contain expected data (3), %d", *Ptr);
            Result = EFAULT;
            goto cleanup;
        }
    }

    //
    // Make sure that the shared buffer contains all shared data.
    //

    Ptr = SharedBuffer;
    EndPtr = Ptr + BufferSize;

    for (; Ptr < EndPtr; Ptr += 1)
    {
        if (*Ptr != 0xDE)
        {
            LxtLogError("Shared buffer not contain expected data (4), %d", *Ptr);
            Result = EFAULT;
            goto cleanup;
        }
    }

    Result = 0;

cleanup:

    if (SharedBuffer != MAP_FAILED)
    {
        munmap(SharedBuffer, BufferSize);
    }

    if (PrivateBuffer != MAP_FAILED)
    {
        munmap(PrivateBuffer, BufferSize);
    }

    if (IsChild)
    {
        _exit(Result);
    }

    return Result;
}

int MadviseVariation2(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int FileId;
    int BufferSize;

    //
    // Initialize locals.
    //

    FileId = -1;
    BufferSize = MADVISE_TEST_BUFFER_SIZE;

    //
    // Run the test on private memory.
    //

    Result = MadviseRunTest(FileId, BufferSize, 0);

    if (Result != 0)
    {
        LxtLogError("Variation2 failed! %d", Result);
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    if (FileId != -1)
    {
        close(FileId);
    }

    return Result;
}

int MadviseVariation3(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int FileId;
    int BufferSize;

    //
    // Initialize locals.
    //

    FileId = -1;
    BufferSize = MADVISE_TEST_BUFFER_SIZE;

    //
    // Run the test on private memory with fork.
    //

    Result = MadviseRunTest(FileId, BufferSize, 1);

    if (Result != 0)
    {
        LxtLogError("Variation3 failed! %d", Result);
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    if (FileId != -1)
    {
        close(FileId);
    }

    return Result;
}

int MadviseVariation4(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int FileId;
    int BufferSize;
    unsigned int WriteData;
    int BytesWritten;
    int ByteIndex;

    //
    // Initialize locals.
    //

    FileId = -1;
    BufferSize = MADVISE_TEST_BUFFER_SIZE;

    //
    // Open the file and fill it to the appropriate size.
    //

    FileId = open("/data/test/madvise_test.bin", O_RDWR | O_CREAT, S_IRWXU);

    if (FileId == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    WriteData = (unsigned int)-1;

    for (ByteIndex = 0; ByteIndex < BufferSize; ByteIndex += sizeof(WriteData))
    {

        BytesWritten = write(FileId, &WriteData, sizeof(WriteData));

        if (BytesWritten != sizeof(WriteData))
        {
            Result = errno;
            LxtLogError("Could not write to file! %d", Result);
            goto ErrorExit;
        }
    }

    //
    // Run the test on file-backed memory.
    //

    Result = MadviseRunTest(FileId, BufferSize, 0);

    if (Result != 0)
    {
        LxtLogError("Variation4 failed! %d", Result);
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    if (FileId != -1)
    {
        close(FileId);
    }

    return Result;
}

int MadviseVariation5(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int FileId;
    int BufferSize;
    unsigned int WriteData;
    int BytesWritten;
    int ByteIndex;

    //
    // Initialize locals.
    //

    FileId = -1;
    BufferSize = MADVISE_TEST_BUFFER_SIZE;

    //
    // Open the file and fill it to the appropriate size.
    //

    FileId = open("/data/test/madvise_test.bin", O_RDWR | O_CREAT, S_IRWXU);

    if (FileId == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    WriteData = (unsigned int)-1;

    for (ByteIndex = 0; ByteIndex < BufferSize; ByteIndex += sizeof(WriteData))
    {

        BytesWritten = write(FileId, &WriteData, sizeof(WriteData));

        if (BytesWritten != sizeof(WriteData))
        {
            Result = errno;
            LxtLogError("Could not write to file! %d", Result);
            goto ErrorExit;
        }
    }

    //
    // Run the test on file-backed memory with fork.
    //

    Result = MadviseRunTest(FileId, BufferSize, 1);

    if (Result != 0)
    {
        LxtLogError("Variation5 failed! %d", Result);
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    if (FileId != -1)
    {
        close(FileId);
    }

    return Result;
}
