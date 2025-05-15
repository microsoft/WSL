/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    mprotect.c

Abstract:

    This file contains test cases for mprotect().

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sched.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define LXT_NAME "mprotect"
#define MAPPING_PAGE_SIZE 4096
#define MAX_BUF_SIZE 256
#define PROCFS_MNT "/proc"

int MprotectMainVariation(PLXT_ARGS Args);

int MunmapMainVariation(PLXT_ARGS Args);

int MsyncMainVariation(PLXT_ARGS Args);

int MadviseMainVariation(PLXT_ARGS Args);

int MremapMainVariation(PLXT_ARGS Args);

int MprotectStackVariation(PLXT_ARGS Args);

static const LXT_VARIATION g_LxtVariations[] = {
    {"mprotect main variation", MprotectMainVariation},
    {"munmap main variation", MunmapMainVariation},
    {"msync main variation", MsyncMainVariation},
    {"madvise main variation", MadviseMainVariation},
    {"mremap main variation", MremapMainVariation},
    {"mprotect stack variation", MprotectStackVariation},
};

int MprotectMainVariation(PLXT_ARGS Args)
{
    int Result;
    int ROFileDescriptor;
    int RWFileDescriptor;
    char FileBuffer[3 * MAPPING_PAGE_SIZE];
    char* ROMapping;
    char* RWMapping;
    int MapSize;
    char* RemappedMemory;

    MapSize = sizeof(FileBuffer);

    ROFileDescriptor = open("/data/mprotect_test.bin", O_RDONLY | O_CREAT | O_TRUNC, S_IRWXU);

    if (ROFileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    RWFileDescriptor = open("/data/mprotect_test.bin", O_RDWR);

    if (RWFileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    write(RWFileDescriptor, FileBuffer, MapSize);

    ROMapping = mmap(NULL, MapSize, PROT_READ, MAP_SHARED, ROFileDescriptor, 0);

    if (ROMapping == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("ROMapping allocation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("ROMapping: %p", ROMapping);

    RWMapping = mmap(NULL, MapSize, PROT_READ, MAP_SHARED, RWFileDescriptor, 0);

    if (RWMapping == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("ROMapping allocation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("RWMapping: %p", RWMapping);

    //
    // Checking behavior of mprotect with zero size mappings.
    //

    LxtLogInfo("Checking mprotect behavior with zero size mappings") LxtCheckErrno(mprotect(NULL, 0, PROT_WRITE));
    LxtCheckErrno(mprotect(NULL, 0, 0xDEADBEEF));
    LxtCheckErrno(mprotect(RWMapping, 0, 0xDEADBEEF));
    LxtCheckErrno(mprotect(ROMapping, 0, PROT_WRITE));
    LxtCheckErrno(mprotect(ROMapping, 0, PROT_READ));
    LxtCheckErrno(mprotect(RWMapping, 0, PROT_WRITE));
    LxtCheckErrno(mprotect(RWMapping, 0, PROT_READ));

    //
    // Check behavior of mprotect with a bad address.
    //

    LxtLogInfo("Checking mprotect behavior with a bad address") LxtCheckErrnoFailure(mprotect(NULL, MAPPING_PAGE_SIZE, PROT_WRITE), ENOMEM);
    LxtCheckErrnoFailure(mprotect(RWMapping + 300, MAPPING_PAGE_SIZE, PROT_WRITE), EINVAL);

    //
    // Check behavior with bad protection flags.
    //

    LxtLogInfo("Checking mprotect behavior with bad protection flags")
        LxtCheckErrnoFailure(mprotect(RWMapping, MAPPING_PAGE_SIZE, 0xDEADBEEF), EINVAL);

    //
    // Checking mprotect on non-zero size mappings.
    //

    Result = mprotect(RWMapping, MAPPING_PAGE_SIZE, PROT_WRITE);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Protection change failed unexpectedly! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("RWMapping protection succeeded");

    Result = mprotect(ROMapping, MAPPING_PAGE_SIZE, PROT_WRITE);

    if (Result != -1)
    {
        LxtLogError("Protection change on RO file succeeded unexpectedly!");
        goto ErrorExit;
    }

    Result = errno;
    if (Result != EACCES)
    {
        LxtLogError("RO protection change failed but not with EACCES! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("ROMapping protection failed as expected");

    RemappedMemory = mremap(ROMapping, MAPPING_PAGE_SIZE, 2 * MAPPING_PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Remap on RO file failed unexpectedly! %d!", Result);
        goto ErrorExit;
    }

    LxtLogInfo("Remapping succeeded");

    RemappedMemory = mremap(RWMapping, MAPPING_PAGE_SIZE, 2 * MAPPING_PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Remap on RO file failed unexpectedly! %d!", Result);
        goto ErrorExit;
    }

    LxtLogInfo("Remapping succeeded");
    LxtLogPassed("Success!");
    Result = 0;

ErrorExit:

    return Result;
}

int MunmapMainVariation(PLXT_ARGS Args)
{
    char FileBuffer[3 * MAPPING_PAGE_SIZE];
    int MapSize;
    int Result;
    int RWFileDescriptor;
    char* RWMapping;

    //
    // Create a file and write garbage data to it.
    //

    MapSize = sizeof(FileBuffer);
    RWFileDescriptor = open("/data/mprotect_test.bin", O_CREAT | O_TRUNC | O_RDWR, S_IRWXU);

    if (RWFileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    write(RWFileDescriptor, FileBuffer, MapSize);

    //
    // Map a memory segment that will be backed by the file.
    //

    RWMapping = mmap(NULL, MapSize, PROT_READ, MAP_SHARED, RWFileDescriptor, 0);

    if (RWMapping == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("ROMapping allocation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("RWMapping: %p", RWMapping);

    //
    // Check different variations of bad arguments with munmap.
    //

    LxtLogInfo("Checking munmap with bad arguments");
    LxtCheckErrnoFailure(munmap(NULL, 0), EINVAL);
    LxtCheckErrnoFailure(munmap(RWMapping, 0), EINVAL);
    LxtCheckErrnoFailure(munmap(RWMapping + 300, MapSize), EINVAL);
    LxtCheckErrno(munmap(NULL, MapSize));

    //
    // Unmap the memory mapping.
    //

    LxtLogInfo("Unmapping the mapping");
    LxtCheckErrno(munmap(RWMapping, MapSize));

    //
    // All tests passed at this point.
    //

    LxtLogPassed("Success!") Result = 0;

ErrorExit:
    if (RWFileDescriptor >= 0)
    {
        if (LxtClose(RWFileDescriptor) < 0)
        {
            LxtLogInfo("Failed to close test file at the end of the test");
        }
    }

    return Result;
}

int MsyncMainVariation(PLXT_ARGS Args)
{
    char FileBuffer[3 * MAPPING_PAGE_SIZE];
    int MapSize;
    int Result;
    int RWFileDescriptor;
    char* RWMapping;

    //
    // Create a file and write garbage data to it.
    //

    MapSize = sizeof(FileBuffer);
    RWFileDescriptor = open("/data/mprotect_test.bin", O_CREAT | O_TRUNC | O_RDWR, S_IRWXU);

    if (RWFileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    write(RWFileDescriptor, FileBuffer, MapSize);

    //
    // Map a memory segment that will be backed by the file.
    //

    RWMapping = mmap(NULL, MapSize, PROT_READ | PROT_WRITE, MAP_SHARED, RWFileDescriptor, 0);

    if (RWMapping == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("ROMapping allocation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("RWMapping: %p", RWMapping);

    //
    // Read the first byte from the file, increment it and write it back.
    //

    *RWMapping = *RWMapping + 1;

    //
    // Checking behavior of msync with zero length.
    //

    LxtLogInfo("Checking msync behavior with zero length") LxtCheckErrno(msync(NULL, 0, MS_SYNC));
    LxtCheckErrno(msync(RWMapping, 0, MS_SYNC));
    LxtCheckErrnoFailure(msync(NULL, 0, 0xDEADBEEF), EINVAL);
    LxtCheckErrnoFailure(msync(RWMapping, 0, 0xDEADBEEF), EINVAL);

    //
    // Check behavior of mprotect with a bad address.
    //

    LxtLogInfo("Checking msync behavior with a bad address") LxtCheckErrnoFailure(msync(NULL, MAPPING_PAGE_SIZE, MS_SYNC), ENOMEM);
    LxtCheckErrnoFailure(msync(RWMapping + 300, MAPPING_PAGE_SIZE, MS_SYNC), EINVAL);

    //
    // Check behavior of msync with bad flags.
    //

    LxtLogInfo("Checking msync behavior with bad protection flags")
        LxtCheckErrnoFailure(msync(RWMapping, MAPPING_PAGE_SIZE, 0xDEADBEEF), EINVAL);

    //
    // Sync the changes.
    //

    LxtLogInfo("Sync the changes to the file");
    LxtCheckErrno(msync(RWMapping, MAPPING_PAGE_SIZE, MS_SYNC));

    //
    // All tests passed at this point.
    //

    LxtLogPassed("Success!") Result = 0;

ErrorExit:
    if (RWFileDescriptor >= 0)
    {
        if (LxtClose(RWFileDescriptor) < 0)
        {
            LxtLogInfo("Failed to close test file at the end of the test");
        }
    }

    return Result;
}

int MadviseMainVariation(PLXT_ARGS Args)
{
    char FileBuffer[3 * MAPPING_PAGE_SIZE];
    int MapSize;
    int Result;
    int RWFileDescriptor;
    char* RWMapping;
    char DummyByte;

    //
    // Create a file and write garbage data to it.
    //

    MapSize = sizeof(FileBuffer);
    RWFileDescriptor = open("/data/mprotect_test.bin", O_CREAT | O_TRUNC | O_RDWR, S_IRWXU);

    if (RWFileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    write(RWFileDescriptor, FileBuffer, MapSize);

    //
    // Map a memory segment that will be backed by the file.
    //

    RWMapping = mmap(NULL, MapSize, PROT_READ | PROT_WRITE, MAP_SHARED, RWFileDescriptor, 0);

    if (RWMapping == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("ROMapping allocation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("RWMapping: %p", RWMapping);

    //
    // Checking behavior of mremap with zero length.
    //

    LxtLogInfo("Checking madvise behavior with zero length") LxtCheckErrno(madvise(NULL, 0, MADV_RANDOM));
    LxtCheckErrno(madvise(RWMapping, 0, MADV_RANDOM));
    LxtCheckErrnoFailure(madvise(NULL, 0, 0xDEADBEEF), EINVAL);
    LxtCheckErrnoFailure(madvise(RWMapping, 0, 0xDEADBEEF), EINVAL);

    //
    // Check behavior of madvise with a bad address.
    //

    LxtLogInfo("Checking madvise behavior with a bad address") LxtCheckErrnoFailure(madvise(NULL, MAPPING_PAGE_SIZE, MADV_RANDOM), ENOMEM);
    LxtCheckErrnoFailure(madvise(RWMapping + 300, MAPPING_PAGE_SIZE, MADV_RANDOM), EINVAL);

    //
    // Check behavior of madvise with bad flags.
    //

    LxtLogInfo("Checking madvise behavior with bad protection flags")
        LxtCheckErrnoFailure(madvise(RWMapping, MAPPING_PAGE_SIZE, 0xDEADBEEF), EINVAL);

    //
    // Advise the kernel on access partners.
    //

    LxtLogInfo("Advise the kernel on access patterns.");
    LxtCheckErrno(madvise(RWMapping, MAPPING_PAGE_SIZE, MADV_RANDOM));

    //
    // All tests passed at this point.
    //

    LxtLogPassed("Success!") Result = 0;

ErrorExit:
    if (RWFileDescriptor >= 0)
    {
        if (LxtClose(RWFileDescriptor) < 0)
        {
            LxtLogInfo("Failed to close test file at the end of the test");
        }
    }

    return Result;
}

int MremapMainVariation(PLXT_ARGS Args)
{
    char FileBuffer[3 * MAPPING_PAGE_SIZE];
    int MapSize;
    char* RemappedMemory;
    int Result;
    int RWFileDescriptor;
    char* RWMapping;
    char DummyByte;

    //
    // Create a file and write garbage data to it.
    //

    MapSize = sizeof(FileBuffer);
    RWFileDescriptor = open("/data/mprotect_test.bin", O_CREAT | O_TRUNC | O_RDWR, S_IRWXU);

    if (RWFileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    write(RWFileDescriptor, FileBuffer, MapSize);

    //
    // Map a memory segment that will be backed by the file.
    //

    RWMapping = mmap(NULL, MapSize, PROT_READ | PROT_WRITE, MAP_SHARED, RWFileDescriptor, 0);

    if (RWMapping == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("ROMapping allocation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("RWMapping: %p", RWMapping);

    //
    // Checking behavior of mremap with bad arguments.
    //

    LxtLogInfo("Checking mremap behavior with bad arguments");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

    LxtCheckErrnoFailure((int)mremap(NULL, MAPPING_PAGE_SIZE, 2 * MAPPING_PAGE_SIZE, MREMAP_MAYMOVE), EFAULT);

    LxtCheckErrnoFailure((int)mremap(RWMapping, MAPPING_PAGE_SIZE, 2 * MAPPING_PAGE_SIZE, 0xDEADBEEF), EINVAL);

    LxtCheckErrnoFailure((int)mremap(RWMapping, MAPPING_PAGE_SIZE, 0, 0xDEADBEEF), EINVAL);

    LxtCheckErrnoFailure((int)mremap(NULL, 0, 2 * MAPPING_PAGE_SIZE, MREMAP_MAYMOVE), EFAULT);

    LxtCheckErrnoFailure((int)mremap(NULL, 0, 0, 0xDEADBEEF), EINVAL);

    LxtCheckErrnoFailure((int)mremap(NULL, 0, 2 * MAPPING_PAGE_SIZE, 0xDEADBEEF), EINVAL);

    LxtCheckErrnoFailure((int)mremap(RWMapping, 0, 0, MREMAP_MAYMOVE), EINVAL);

    LxtCheckErrnoFailure((int)mremap(NULL, 0, 0, MREMAP_MAYMOVE), EINVAL);

    LxtCheckErrnoFailure((int)mremap(NULL, MAPPING_PAGE_SIZE, 2 * MAPPING_PAGE_SIZE, 0xDEADBEEF), EINVAL);

    LxtCheckErrnoFailure((int)mremap(RWMapping + 300, MAPPING_PAGE_SIZE, 2 * MAPPING_PAGE_SIZE, MREMAP_MAYMOVE), EINVAL);

    LxtCheckErrnoFailure((int)mremap(RWMapping, MAPPING_PAGE_SIZE, 0, MREMAP_MAYMOVE), EINVAL);

#pragma GCC diagnostic pop

    //
    // Success cases.
    //

    //
    // Shrink the mapped memory.
    //

    RemappedMemory = (char*)mremap(RWMapping, 3 * MAPPING_PAGE_SIZE, 2 * MAPPING_PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {
        LxtLogInfo("Mapping Failed");
        Result = errno;
        goto ErrorExit;
    }

    LxtLogInfo("RemappedMemory = %p", RemappedMemory);

    //
    // All tests passed at this point.
    //

    LxtLogPassed("Success!") Result = 0;

ErrorExit:
    if (RWFileDescriptor >= 0)
    {
        if (LxtClose(RWFileDescriptor) < 0)
        {
            LxtLogInfo("Failed to close test file at the end of the test");
        }
    }

    return Result;
}

int MprotectStackVariation(PLXT_ARGS Args)

{

    void* Address;
    int Result;
    char StackBuffer[20000];

    //
    // Ensure PROT_GROWSDOWN doesn't work on normal allocations.
    //

    Address = mmap(NULL, 4096, PROT_NONE, MAP_ANONYMOUS, -1, 0);
    LxtCheckErrnoFailure(mprotect(Address, 4096, PROT_READ | PROT_WRITE | PROT_GROWSDOWN), EINVAL);
    munmap(Address, 4096);

    //
    // Make sure PROT_GROWSDOWN works on the stack. There's not an easy
    // way to validate that it actually worked without parsing /proc/self/maps...
    //

    StackBuffer[0] = 'x';
    Address = (void*)(((long)StackBuffer + sizeof(StackBuffer)) & ~4095);
    LxtCheckErrno(mprotect(Address, 4096, PROT_READ | PROT_WRITE | PROT_GROWSDOWN));

    Result = 0;

ErrorExit:
    return Result;
}

int MprotectTestEntry(int Argc, char* Argv[])
{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return LXT_RESULT_SUCCESS;
}
