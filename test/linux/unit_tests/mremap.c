/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    mremap.c

Abstract:

    This file contains test cases for mremap().

--*/

#include "lxtcommon.h"
#include "unittests.h"
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
#include <pthread.h>

#define LXT_NAME "mremap"

void FillMemory(char* Memory, int Size, unsigned char Value)
{
    int i;

    LxtLogInfo("FillMemory: %p, 0x%x, %u", Memory, Size, Value);

    for (i = 0; i < Size; i += 1)
    {

        if ((i != 0) && ((i % PAGE_SIZE) == 0))
        {
            Value += 1;
        }

        Memory[i] = Value;
    }

    return;
}

int CheckMemory(unsigned char* Memory, int Size, unsigned char Value)
{
    int i;

    LxtLogInfo("CheckMemory: %p, 0x%x, %u", Memory, Size, Value);

    for (i = 0; i < Size; i += 1)
    {

        if ((i != 0) && ((i % PAGE_SIZE) == 0))
        {
            Value += 1;
        }

        if (Memory[i] != Value)
        {
            LxtLogError("Mismatched byte %d! Value: %d", i, (int)Memory[i]);
            break;
        }
    }

    return i;
}

void* ThreadWorker(void* Context)
{
    char* Memory;
    int Result;

    LxtLogInfo("Thread started.");

    for (;;)
    {

        Memory = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

        if (Memory == MAP_FAILED)
        {
            Result = errno;
            LxtLogError("Thread memory allocation failed! %d", Result);
            goto ErrorExit;
        }

        munmap(Memory, PAGE_SIZE);
    }

ErrorExit:

    return NULL;
}

int MremapTestEntry(int Argc, char* Argv[])
{
    LXT_ARGS Args;
    char* PrivateMemory;
    char* SharedPrivateMemory;
    char* FileMemory;
    char* FileMemory2;
    char* RemappedMemory;
    char* SpanMemory1;
    char* SpanMemory2;
    char* SpanMemory3;
    int Result;
    int FileDescriptor;
    int DevZeroDescriptor;
    int AllocationSize;
    char FileBuffer[3 * PAGE_SIZE];
    int FailByte;
    int MapsDescriptor;
    int BytesRead;
    char* MapsBuffer;
    pthread_t Thread;
    int FileDescriptor2;
    int FileSize;
    struct timespec Time;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));

    LxtLogStart("Start Prep:");

    Result = pthread_create(&Thread, NULL, ThreadWorker, NULL);

    if (Result != 0)
    {
        LxtLogError("Thread creation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("Thread created.");

    AllocationSize = 2 * PAGE_SIZE;

    PrivateMemory = mmap(NULL, AllocationSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (PrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("PrivateMemory allocation failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(PrivateMemory, AllocationSize, 1);

    SharedPrivateMemory = mmap(NULL, AllocationSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);

    if (SharedPrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("SharedPrivateMemory allocation failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(SharedPrivateMemory, AllocationSize, 10);

    FileDescriptor = open("/data/test.bin", O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);

    if (FileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    write(FileDescriptor, FileBuffer, sizeof(FileBuffer));

    FileMemory = mmap(NULL, AllocationSize, PROT_READ | PROT_WRITE, MAP_SHARED, FileDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("FileMemory allocation failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory, AllocationSize, 20);
    SpanMemory3 = mmap(NULL, AllocationSize * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);

    if (SpanMemory3 == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("SpanMemory1 allocation failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(SpanMemory3, AllocationSize * 3, 60);

    SpanMemory1 = mmap(SpanMemory3, AllocationSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, 0, 0);

    if (SpanMemory1 == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("SpanMemory1 allocation failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(SpanMemory1, AllocationSize, 40);

    SpanMemory2 =
        mmap(SpanMemory1 + AllocationSize, AllocationSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, 0, 0);

    if (SpanMemory2 == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("SpanMemory2 allocation failed! %d", Result);
        goto ErrorExit;
    }

    if (SpanMemory2 != (SpanMemory1 + AllocationSize))
    {
        LxtLogError("SpanMemory2 allocation isn't in the right place!");
        goto ErrorExit;
    }

    FillMemory(SpanMemory2, AllocationSize, 50);
    LxtLogPassed("Prep complete!");

    LxtLogStart("Start Test Cases:");

    //
    // Case 1: Extend private memory.
    //

    RemappedMemory = mremap(PrivateMemory, AllocationSize, AllocationSize + PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 1 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 1 succeeded. %p -> %p", PrivateMemory, RemappedMemory);
        PrivateMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, AllocationSize, 1);

        if (FailByte != AllocationSize)
        {
            LxtLogError("Case 1 memory doesn't match at byte %d!!!", FailByte);
        }

        FillMemory(RemappedMemory + AllocationSize, PAGE_SIZE, 3);
    }

    //
    // Case 3: Extend file mapping.
    //

    RemappedMemory = mremap(FileMemory, AllocationSize, AllocationSize + PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 3 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 3 succeeded. %p -> %p", FileMemory, RemappedMemory);
        FileMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, AllocationSize, 20);

        if (FailByte != AllocationSize)
        {
            LxtLogError("Case 3 memory doesn't match at byte %d!!!", FailByte);
        }

        FillMemory(RemappedMemory + AllocationSize, PAGE_SIZE, 22);
    }

    //
    // Case 7: Move range that spans two private allocations (same type).
    //

    RemappedMemory = mremap(SpanMemory1 + PAGE_SIZE, AllocationSize, AllocationSize + (2 * PAGE_SIZE), MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 7 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 7 succeeded. %p -> %p", SpanMemory1 + PAGE_SIZE, RemappedMemory);

        FailByte = CheckMemory(RemappedMemory, PAGE_SIZE, 41);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 7 first page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + PAGE_SIZE, PAGE_SIZE, 50);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 7 second page memory doesn't match at byte %d!!! V: %d", FailByte);
        }
    }

    //
    // Case 8: Move range that spans two different allocations (private and shared anonymous).
    //

    RemappedMemory = mremap(SpanMemory2 + PAGE_SIZE, AllocationSize, AllocationSize + (2 * PAGE_SIZE), MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogPassed("Case 8 failed as expected. %d", Result);
    }
    else
    {

        LxtLogError("Case 8 succeeded not expected! %p -> %p", SpanMemory2 + PAGE_SIZE, RemappedMemory);

        FailByte = CheckMemory(RemappedMemory, PAGE_SIZE, 51);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 8 first page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + PAGE_SIZE, PAGE_SIZE, 60);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 8 second page memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 9: Shrink private allocation.
    //

    RemappedMemory = mremap(PrivateMemory, AllocationSize + PAGE_SIZE, AllocationSize, 0);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 9 failed! %d", Result);
    }
    else
    {
        LxtLogPassed("Case 9 succeeded. %p -> %p", PrivateMemory, RemappedMemory);
        PrivateMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, AllocationSize, 1);

        if (FailByte != AllocationSize)
        {
            LxtLogError("Case 9 memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 10: Move and extend with different protections.
    //

    Result = mprotect(PrivateMemory, PAGE_SIZE, PROT_READ);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Case 10 protection change failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(PrivateMemory, AllocationSize, AllocationSize + PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogPassed("Case 10 failed as expected. %d", Result);
    }
    else
    {
        LxtLogError("Case 10 succeeded not expected!. %p -> %p", PrivateMemory, RemappedMemory);
        PrivateMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, AllocationSize, 1);

        if (FailByte != AllocationSize)
        {
            LxtLogError("Case 10 memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 11: Move discontiguous (by mapping offset not VA) file mappings.
    //

    FileMemory = mmap(NULL, AllocationSize, PROT_READ | PROT_WRITE, MAP_SHARED, FileDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 11 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory, AllocationSize, 70);

    RemappedMemory = mmap(FileMemory + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, FileDescriptor, 0x2000);

    if (RemappedMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 11 map2 failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(FileMemory, AllocationSize, AllocationSize + (2 * PAGE_SIZE), MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogPassed("Case 11 failed as expected. %d", Result);
    }
    else
    {

        LxtLogError("Case 11 succeeded not expected!. %p -> %p", FileMemory, RemappedMemory);

        FailByte = CheckMemory(RemappedMemory, AllocationSize, 70);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 11 memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 12: Extend within existing VAD (section).
    //

    FileMemory = mmap(NULL, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FileDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 12 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory, 3 * PAGE_SIZE, 80);

    RemappedMemory = mremap(FileMemory, PAGE_SIZE, 3 * PAGE_SIZE, 0);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogPassed("Case 12 failed as expected. %d", Result);
    }
    else
    {

        LxtLogError("Case 12 succeeded not expected! %p -> %p", FileMemory, RemappedMemory);

        FailByte = CheckMemory(RemappedMemory, 3 * PAGE_SIZE, 80);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 12 memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 13: Extend within existing VAD (private).
    //

    PrivateMemory = mmap(NULL, PAGE_SIZE * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (PrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 13 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(PrivateMemory, 3 * PAGE_SIZE, 90);

    RemappedMemory = mremap(PrivateMemory, PAGE_SIZE, 3 * PAGE_SIZE, 0);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogPassed("Case 13 failed as expected. %d", Result);
    }
    else
    {
        LxtLogError("Case 13 succeeded not expected! %p -> %p", FileMemory, RemappedMemory);
    }

    //
    // Case 14: Split VAD during remap shrinking.
    //

    RemappedMemory = mremap(PrivateMemory, 2 * PAGE_SIZE, PAGE_SIZE, 0);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 14 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 14 succeeded. %p -> %p", PrivateMemory, RemappedMemory);
        PrivateMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, PAGE_SIZE, 90);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 14 first page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + (2 * PAGE_SIZE), PAGE_SIZE, 92);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 14 third page memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 15: Partially committed and no-access private memory.
    //

    PrivateMemory = mmap(NULL, PAGE_SIZE * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (PrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 15 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(PrivateMemory, PAGE_SIZE, 100);
    FillMemory(PrivateMemory + (2 * PAGE_SIZE), PAGE_SIZE, 102);

    Result = mprotect(PrivateMemory, PAGE_SIZE * 3, PROT_NONE);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Case 15 protection change failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(PrivateMemory, 3 * PAGE_SIZE, 33 * PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 15 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 15 succeeded. %p -> %p", PrivateMemory, RemappedMemory);
        PrivateMemory = RemappedMemory;

        Result = mprotect(PrivateMemory, (33 * PAGE_SIZE), PROT_READ | PROT_WRITE);

        if (Result == -1)
        {
            Result = errno;
            LxtLogError("Case 15 protection change failed! %d", Result);
            goto ErrorExit;
        }

        FailByte = CheckMemory(RemappedMemory, PAGE_SIZE, 100);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 15 first page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + (2 * PAGE_SIZE), PAGE_SIZE, 102);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 15 third page memory doesn't match at byte %d!!!", FailByte);
        }

        FillMemory(PrivateMemory + (3 * PAGE_SIZE), (30 * PAGE_SIZE), 103);
    }

    //
    // Case 16: Remap private and shared file mapping of the same file.
    //

    FileMemory = mmap(NULL, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FileDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 16 first page map failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mmap(FileMemory + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, FileDescriptor, PAGE_SIZE);

    if (RemappedMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 16 second page map failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(FileMemory, 2 * PAGE_SIZE, 3 * PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogPassed("Case 16 failed as expected. %d", Result);
    }
    else
    {
        LxtLogError("Case 16 succeeded not expected! %p -> %p", FileMemory, RemappedMemory);
    }

    //
    // Case 17: Remap private section view with no pages copy-on-written.
    //

    write(FileDescriptor, FileBuffer, PAGE_SIZE);

    FileMemory = mmap(NULL, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, FileDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 17 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory, 4 * PAGE_SIZE, 110);

    FileMemory = mmap(FileMemory, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, FileDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 17 private map failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(FileMemory, 2 * PAGE_SIZE, 4 * PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 17 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 17 succeeded. %p -> %p", FileMemory, RemappedMemory);
        FileMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, 4 * PAGE_SIZE, 110);

        if (FailByte != (4 * PAGE_SIZE))
        {
            LxtLogError("Case 17 memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 18: Remap private section view with some pages copy-on-written.
    //

    FillMemory((FileMemory + PAGE_SIZE), PAGE_SIZE, 120);

    RemappedMemory = mremap(FileMemory, 3 * PAGE_SIZE, 4 * PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 18 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 18 succeeded. %p -> %p", FileMemory, RemappedMemory);
        FileMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, PAGE_SIZE, 110);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 18 first page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + PAGE_SIZE, PAGE_SIZE, 120);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 18 second page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + (2 * PAGE_SIZE), (2 * PAGE_SIZE), 112);

        if (FailByte != (2 * PAGE_SIZE))
        {
            LxtLogError("Case 18 third/fourth page memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 19: Large region of copy-on-written pages with same protection.
    //

    FileSize = (512 * 1024);

    FileDescriptor2 = open("/data/test2.bin", O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);

    if (FileDescriptor2 == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto ErrorExit;
    }

    for (FailByte = 0; FailByte < (FileSize / PAGE_SIZE); FailByte += 1)
    {
        write(FileDescriptor2, FileBuffer, PAGE_SIZE);
    }

    FileMemory = mmap(NULL, FileSize, PROT_READ | PROT_WRITE, MAP_SHARED, FileDescriptor2, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 19 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory, FileSize, 0);

    FileMemory = mmap(FileMemory, FileSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, FileDescriptor2, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 19 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory + (3 * PAGE_SIZE), FileSize - (6 * PAGE_SIZE), 130);

    Result = mprotect(FileMemory, FileSize, PROT_READ);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Case 19 protection change failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(FileMemory, FileSize - PAGE_SIZE, FileSize, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 19 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 19 succeeded. %p -> %p", FileMemory, RemappedMemory);
        FileMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, (3 * PAGE_SIZE), 0);

        if (FailByte != (3 * PAGE_SIZE))
        {
            LxtLogError("Case 19 first pages memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + (3 * PAGE_SIZE), FileSize - (6 * PAGE_SIZE), 130);

        if (FailByte != FileSize - (6 * PAGE_SIZE))
        {
            LxtLogError("Case 19 middle pages memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + FileSize - (3 * PAGE_SIZE), (3 * PAGE_SIZE), ((FileSize / PAGE_SIZE) - 3));

        if (FailByte != (3 * PAGE_SIZE))
        {
            LxtLogError("Case 19 third pages memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 20: Remap an inconsistent DONT_FORK range.
    //

    PrivateMemory = mmap(NULL, PAGE_SIZE * 3, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (PrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 20 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(PrivateMemory, PAGE_SIZE * 3, 140);

    Result = madvise(PrivateMemory, 1 * PAGE_SIZE, MADV_DONTFORK);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Case 20 madvise failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(PrivateMemory, PAGE_SIZE * 2, PAGE_SIZE * 3, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogPassed("Case 20 failed as expected. %d", Result);
    }
    else
    {

        LxtLogError("Case 20 succeeded not expected! %p -> %p", PrivateMemory, RemappedMemory);
        PrivateMemory = RemappedMemory;
    }

    //
    // Case 21: Remap a DONT_FORK range.
    //

    Result = madvise(PrivateMemory, 3 * PAGE_SIZE, MADV_DONTFORK);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Case 21 madvise failed! %d", Result);
        goto ErrorExit;
    }

    RemappedMemory = mremap(PrivateMemory, PAGE_SIZE * 2, PAGE_SIZE * 3, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 21 failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 21 succeeded. %p -> %p", PrivateMemory, RemappedMemory);
        PrivateMemory = RemappedMemory;
    }

    //
    // \Dev\Zero tests.
    //

    DevZeroDescriptor = open("/dev/zero", O_RDWR);

    if (DevZeroDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not open \\dev\\zero device! %d", Result);
        goto ErrorExit;
    }

    memset(FileBuffer, 1, PAGE_SIZE);

    Result = write(DevZeroDescriptor, FileBuffer, PAGE_SIZE);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Failed to write to \\dev\\zero! %d", Result);
    }
    else
    {
        LxtLogInfo("Write to \\dev\\zero result: %d", Result);
    }

    memset(FileBuffer, 5, PAGE_SIZE);

    Result = read(DevZeroDescriptor, FileBuffer, PAGE_SIZE);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Failed to read from \\dev\\zero! %d", Result);
    }
    else
    {

        FailByte = CheckMemory(FileBuffer, PAGE_SIZE, 0);

        if (FailByte == PAGE_SIZE)
        {
            LxtLogInfo("Read all zeroes from \\dev\\zero as expected.");
        }
    }

    //
    // Case 22: Remap private \dev\zero mapping.
    //

    FileMemory = mmap(NULL, 1 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE, DevZeroDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 22 first map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory, 50 * PAGE_SIZE, 1);

    RemappedMemory = mremap(FileMemory, 100 * PAGE_SIZE, 10 * 1024 * 1024, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 22 remap failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 22 succeeded. %p -> %p", FileMemory, RemappedMemory);
        FileMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, (50 * PAGE_SIZE), 1);

        if (FailByte != (50 * PAGE_SIZE))
        {
            LxtLogError("Case 22 first pages memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + (50 * PAGE_SIZE), PAGE_SIZE, 0);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 22 second pages not zero!");
        }

        FillMemory(RemappedMemory, 10 * 1024 * 1024, 1);

        FailByte = CheckMemory(FileMemory, 10 * 1024 * 1024, 1);

        if (FailByte != (10 * 1024 * 1024))
        {
            LxtLogError("Case 22 memory doesn't match!");
        }
    }

    //
    // Case 23: Second private mapping of \dev\zero of same file descriptor
    //          does not share memory with the first private mapping.
    //

    FileMemory2 = mmap(NULL, 2 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE, DevZeroDescriptor, 0);

    if (FileMemory2 == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 23 map failed! %d", Result);
        goto ErrorExit;
    }

    FailByte = CheckMemory(FileMemory2, PAGE_SIZE, 0);

    if (FailByte != PAGE_SIZE)
    {
        LxtLogError("Case 23 expected second private mapping to be full of zeroes!");
    }

    //
    // Case 24: Remap shared \dev\zero mapping.
    //

    FileMemory = mmap(NULL, 1 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, DevZeroDescriptor, 0);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 24 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(FileMemory, 50 * PAGE_SIZE, 1);

    RemappedMemory = mremap(FileMemory, 100 * PAGE_SIZE, 1 * 1024 * 1024, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 24 remap failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 24 succeeded. %p -> %p", FileMemory, RemappedMemory);
        FileMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, (50 * PAGE_SIZE), 1);

        if (FailByte != (50 * PAGE_SIZE))
        {
            LxtLogError("Case 24 first pages memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(RemappedMemory + (50 * PAGE_SIZE), PAGE_SIZE, 0);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 24 second pages not zero!");
        }

        FailByte = CheckMemory(RemappedMemory + (100 * PAGE_SIZE), PAGE_SIZE, 0);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 24 third pages not zero!");
        }

        FillMemory(RemappedMemory, 1 * 1024 * 1024, 1);

        FailByte = CheckMemory(FileMemory, 1 * 1024 * 1024, 1);

        if (FailByte != (1 * 1024 * 1024))
        {
            LxtLogError("Case 24 memory doesn't match!");
        }
    }

    //
    // Case 25: Remap shared \dev\zero mapping smaller.
    //

    RemappedMemory = mremap(FileMemory, 1 * 1024 * 1024, PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 25 remap failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 25 succeeded. %p -> %p", FileMemory, RemappedMemory);
        FileMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, PAGE_SIZE, 1);

        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 25 page memory doesn't match at byte %d!!!", FailByte);
        }
    }

    //
    // Case 26: Map \dev\zero with a section offset.
    //

    FileMemory = mmap(NULL, 20 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE, DevZeroDescriptor, 0x5000);

    if (FileMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 26 first map failed! %d", Result);
        goto ErrorExit;
    }
    FillMemory(FileMemory, (20 * 1024 * 1024), 1);

    LxtLogPassed("Case 26 succeeded.");

    //
    // Case 27: Remap shared anonymous mapping.
    //

    SharedPrivateMemory = mmap(NULL, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);

    if (SharedPrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 27 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(SharedPrivateMemory, (2 * PAGE_SIZE), 10);

    RemappedMemory = mremap(SharedPrivateMemory, 2 * PAGE_SIZE, 3 * PAGE_SIZE, MREMAP_MAYMOVE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 27 remap failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 27 succeeded. %p -> %p", SharedPrivateMemory, RemappedMemory);
        SharedPrivateMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, (2 * PAGE_SIZE), 10);

        if (FailByte != (2 * PAGE_SIZE))
        {
            LxtLogError("Case 27 page memory doesn't match at byte %d!!!", FailByte);
        }

        FillMemory(RemappedMemory + (2 * PAGE_SIZE), PAGE_SIZE, 12);
    }

    //
    // Case 28: Remap while chopping off tail.
    //

    SharedPrivateMemory = mmap(NULL, 10 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (SharedPrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 28 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(SharedPrivateMemory, (10 * PAGE_SIZE), 10);

    RemappedMemory = (char*)LxtMremap(
        SharedPrivateMemory, 3 * PAGE_SIZE, 20 * PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, SharedPrivateMemory + (5 * PAGE_SIZE));

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 28 remap failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 28 succeeded. %p -> %p", SharedPrivateMemory, RemappedMemory);
        SharedPrivateMemory = RemappedMemory;

        FailByte = CheckMemory(RemappedMemory, (3 * PAGE_SIZE), 10);

        if (FailByte != (3 * PAGE_SIZE))
        {
            LxtLogError("Case 28 page memory doesn't match at byte %d!!!", FailByte);
        }

        FillMemory(RemappedMemory + (3 * PAGE_SIZE), (17 * PAGE_SIZE), 13);
    }

    //
    // Case 29: Remap while splitting source VAD.
    //

    SharedPrivateMemory = mmap(NULL, 10 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (SharedPrivateMemory == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("Case 29 map failed! %d", Result);
        goto ErrorExit;
    }

    FillMemory(SharedPrivateMemory, (10 * PAGE_SIZE), 10);

    RemappedMemory = (char*)LxtMremap(
        SharedPrivateMemory + (5 * PAGE_SIZE), 4 * PAGE_SIZE, 2 * PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, SharedPrivateMemory + PAGE_SIZE);

    if (RemappedMemory == MAP_FAILED)
    {

        Result = errno;
        LxtLogError("Case 29 remap failed! %d", Result);
    }
    else
    {

        LxtLogPassed("Case 29 succeeded. %p -> %p", SharedPrivateMemory, RemappedMemory);

        FailByte = CheckMemory(SharedPrivateMemory, PAGE_SIZE, 10);
        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 29 first page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(SharedPrivateMemory + PAGE_SIZE, (2 * PAGE_SIZE), 15);
        if (FailByte != (2 * PAGE_SIZE))
        {
            LxtLogError("Case 29 second pages memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(SharedPrivateMemory + (3 * PAGE_SIZE), (2 * PAGE_SIZE), 13);
        if (FailByte != (2 * PAGE_SIZE))
        {
            LxtLogError("Case 29 third page memory doesn't match at byte %d!!!", FailByte);
        }

        FailByte = CheckMemory(SharedPrivateMemory + (9 * PAGE_SIZE), PAGE_SIZE, 19);
        if (FailByte != PAGE_SIZE)
        {
            LxtLogError("Case 29 last page memory doesn't match at byte %d!!!", FailByte);
        }
    }

    fflush(NULL);

    Thread = fork();

    if (Thread == -1)
    {
        Result = errno;
        LxtLogError("Case 20 fork failed! %d", Result);
        goto ErrorExit;
    }

    if (Thread == 0)
    {
        LxtLogInfo("Child");
    }
    else
    {
        usleep(1000 * 500);
        LxtLogInfo("Parent slept");
    }

    MapsBuffer = mmap(NULL, (10 * 1024 * 1024), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

    if (MapsBuffer == MAP_FAILED)
    {
        Result = errno;
        LxtLogError("MapsBuffer allocation failed! %d", Result);
        goto ErrorExit;
    }

    LxtLogInfo("Maps (%p):", MapsBuffer);

    if (Thread == 0)
    {
        LxtLogPassed("Child Done!");
    }
    else
    {
        LxtLogPassed("Parent Done!");
    }

ErrorExit:
    LxtUninitialize();
    return Result;
}
