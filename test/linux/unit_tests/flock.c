/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    flock.c

Abstract:

    This file is a flock test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define LXT_NAME "Flock"

int FnctlLockingVariation0(PLXT_ARGS Args);

int FlockVariation0(PLXT_ARGS Args);

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {{"Flock", FlockVariation0}, {"Fcntl Locking", FnctlLockingVariation0}};

int FlockTestEntry(int Argc, char* Argv[])

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

int FnctlLockingVariation0(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests file locking through the FCNTL system call.

Arguments:

    Args - Supplies a pointer to the test arguments.

Return Value:

    0 on success, -1 on failure.

--*/

{

    char Buffer[10];
    char ByteAlignedBuffer[sizeof(struct flock) + sizeof(struct flock64)];
    int ChildPid;
    int FileDescriptor;
    struct flock* LockDescriptor;
    struct flock64* LockDescriptor64;
    int Result;

    FileDescriptor = -1;
    ChildPid = -1;

    //
    // Initialize the file for this test.
    //

    LxtCheckErrno(FileDescriptor = open("/data/test/fcntl_lock_test.bin", O_RDWR | O_CREAT, S_IRWXU));

    LxtCheckErrno(write(FileDescriptor, Buffer, 10));

    //
    // Set the lock descriptor.
    //

    memset(ByteAlignedBuffer, 0, sizeof(ByteAlignedBuffer));
    LockDescriptor = (struct flock*)&ByteAlignedBuffer[1];
    LockDescriptor->l_type = F_WRLCK;
    LockDescriptor->l_whence = SEEK_SET;
    LockDescriptor->l_start = 0;
    LockDescriptor->l_len = 10;

    //
    // Get the current lock state.
    //

    LxtLogInfo("Fnctl locking - Checking that lock can be set.");
    LxtCheckErrno(fcntl(FileDescriptor, F_GETLK, LockDescriptor));
    LxtCheckEqual(LockDescriptor->l_type, F_UNLCK, "%x");

    //
    // Set the lock.
    //

    LxtLogInfo("Fcntl locking - Setting the read lock by the parent process");
    memset(ByteAlignedBuffer, 0, sizeof(ByteAlignedBuffer));
    LockDescriptor->l_type = F_RDLCK;
    LxtCheckErrno(fcntl(FileDescriptor, F_SETLK, LockDescriptor));

    //
    // Now change the lock to be a write lock.
    //

    memset(ByteAlignedBuffer, 0, sizeof(ByteAlignedBuffer));
    LockDescriptor64 = (struct flock64*)&ByteAlignedBuffer[1];
    LockDescriptor64->l_type = F_WRLCK;
    LockDescriptor64->l_whence = SEEK_SET;
    LockDescriptor64->l_start = 0;
    LockDescriptor64->l_len = 10;

    //
    // Set the lock.
    //

    LxtLogInfo("Fcntl locking - Setting the write lock with 64 bit set lock");
    LxtCheckErrno(fcntl(FileDescriptor, F_SETLK64, LockDescriptor64));

    //
    // Fork the process.
    //

    LxtLogInfo("Creating child process to test the lock.");
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Ensure that the lock was correctly set before. The lock descriptor is
        // correctly set from before. The test expects to see the lock type be
        // changed to exclusive, WRLCK, though.
        //

        LxtLogInfo("Fcntl child locking - Reading lock type");
        LxtCheckErrno(fcntl(FileDescriptor, F_GETLK, LockDescriptor));
        LxtCheckEqual(LockDescriptor->l_type, F_WRLCK, "%X");

        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    Result = LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS);

ErrorExit:
    if (FileDescriptor != 0)
    {
        close(FileDescriptor);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int FlockVariation0(PLXT_ARGS Args)

/*++
--*/

{
    int Result;
    int FileDescriptor1;
    int FileDescriptor2;
    int FileDescriptor3;
    int DupedDescriptor;
    int ChildPid;
    int Index;

    //
    // Initialize locals.
    //

    FileDescriptor1 = -1;
    FileDescriptor2 = -1;
    FileDescriptor3 = -1;
    DupedDescriptor = -1;
    ChildPid = -1;

    //
    // Open a file that will be locked.
    //

    FileDescriptor1 = open("/data/test/flock_test.bin", O_RDWR | O_CREAT, S_IRWXU);

    if (FileDescriptor1 == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto cleanup;
    }

    FileDescriptor2 = open("/data/test/flock_test.bin", O_RDWR | O_CREAT, S_IRWXU);

    if (FileDescriptor2 == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto cleanup;
    }

    Result = flock(FileDescriptor1, LOCK_EX);

    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Lock failed! %d", Result);
        goto cleanup;
    }

    FileDescriptor3 = open("/data/test/flock_test.bin", O_RDWR | O_CREAT, S_IRWXU);

    if (FileDescriptor3 == -1)
    {
        Result = errno;
        LxtLogError("Could not create test file! %d", Result);
        goto cleanup;
    }

    //
    // Lock the file from another descriptor (non-blocking) and it should fail
    // accordingly.
    //

    Result = flock(FileDescriptor2, LOCK_EX | LOCK_NB);

    if (Result == 0)
    {
        Result = -1;
        LxtLogError("Lock succeeded but should have failed!");
        goto cleanup;
    }

    Result = errno;
    if (Result != EWOULDBLOCK)
    {
        LxtLogError("Lock failed but with wrong error! %d", Result);
        goto cleanup;
    }

    //
    // Dupe the owner descriptor and lock the file shared. This should convert
    // the file to shared.
    //

    DupedDescriptor = dup(FileDescriptor1);

    if (DupedDescriptor < 0)
    {
        Result = errno;
        LxtLogError("Could not dup the descriptor! %d", Result);
        goto cleanup;
    }

    for (Index = 0; Index < 2; Index += 1)
    {

        Result = flock(DupedDescriptor, LOCK_EX);

        if (Result < 0)
        {
            Result = errno;
            LxtLogError("Lock exclusive conversion failed! %d", Result);
            goto cleanup;
        }

        Result = flock(DupedDescriptor, LOCK_SH);

        if (Result < 0)
        {
            Result = errno;
            LxtLogError("Lock shared conversion failed! %d", Result);
            goto cleanup;
        }
    }

    //
    // The lock is now owned shared by descriptor1 (via duped descriptor) so now
    // descriptor 2 should be able to acquire it shared.
    //

    Result = flock(FileDescriptor2, LOCK_SH | LOCK_NB);

    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Lock shared failed for descriptor2! %d", Result);
        goto cleanup;
    }

    //
    // Unlock via descriptor1. That leaves just descriptor2 shared.
    //

    Result = flock(FileDescriptor1, LOCK_UN);

    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Unlock failed for descriptor1! %d", Result);
        goto cleanup;
    }

    //
    // Fork to create another thread to wait for lock.
    //

    for (Index = 0; Index < 2; Index += 1)
    {

        ChildPid = fork();

        if (ChildPid == -1)
        {
            Result = errno;
            LxtLogError("Fork failed! %d", Result);
            goto cleanup;
        }

        if (ChildPid == 0)
        {
            int FileDescriptor;

            if (Index == 0)
            {
                FileDescriptor = FileDescriptor1;
            }
            else
            {
                FileDescriptor = FileDescriptor3;
            }

            //
            // Wait for exclusive lock.
            //

            close(FileDescriptor2);
            FileDescriptor2 = -1;

            LxtLogInfo("C%u: Waiting for lock on FileDescriptor...", Index);

            Result = flock(FileDescriptor, LOCK_EX);

            if (Result < 0)
            {
                Result = errno;
                LxtLogError("Lock acquire failed for descriptor1! %d", Result);
                goto cleanup;
            }

            LxtLogInfo("C%u: Lock acquired on FileDescriptor...", Index);

            Result = flock(FileDescriptor, LOCK_UN);

            if (Result < 0)
            {
                Result = errno;
                LxtLogError("Unlock failed for descriptor1! %d", Result);
                goto cleanup;
            }

            LxtLogInfo("C%u: Sleeping 3 secs...", Index);

            usleep(3 * 1000 * 1000);

            LxtLogInfo("C%u: Waiting for lock shared on FileDescriptor...", Index);

            Result = flock(FileDescriptor, LOCK_SH);

            if (Result < 0)
            {
                Result = errno;
                LxtLogError("Lock acquire failed for descriptor1! %d", Result);
                goto cleanup;
            }

            LxtLogInfo("C%u: Lock acquired on FileDescriptor...", Index);

            Result = flock(FileDescriptor, LOCK_UN);

            if (Result < 0)
            {
                Result = errno;
                LxtLogError("Unlock failed for descriptor1! %d", Result);
                goto cleanup;
            }

            if (Index == 0)
            {
                Result = LXT_RESULT_SUCCESS;
                goto cleanup;
            }

            LxtLogInfo("C%u: Sleeping 3 secs...", Index);

            usleep(3 * 1000 * 1000);

            LxtLogInfo("C%u: Waiting for lock exclusive to be terminated...", Index);

            Result = flock(FileDescriptor, LOCK_EX);

            if (Result == 0)
            {
                Result = -1;
                LxtLogError("Lock acquisition succeeded but EINTR expected!");
                goto cleanup;
            }

            Result = errno;

            if (Result != EINTR)
            {
                LxtLogError("Lock acquisition failed but not with EINTR! %d", Result);
                goto cleanup;
            }

            Result = LXT_RESULT_SUCCESS;

            goto cleanup;
        }
    }
    LxtLogInfo("P: Waiting 3 seconds before releasing lock shared...");

    usleep(3 * 1000 * 1000);

    Result = flock(FileDescriptor2, LOCK_UN);

    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Unlock failed for descriptor2! %d", Result);
        goto cleanup;
    }

    usleep(1 * 1000 * 1000);

    LxtLogInfo("P: Waiting to acquire lock exclusive...");

    Result = flock(FileDescriptor2, LOCK_EX);

    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Lock exclusive failed for descriptor2! %d", Result);
        goto cleanup;
    }

    LxtLogInfo("P: Acquired lock exclusive.");

    LxtLogInfo("P: Sleeping 5 secs...");

    usleep(5 * 1000 * 1000);

    LxtLogInfo("P: Releasing lock exclusive...");
    Result = flock(FileDescriptor2, LOCK_UN);

    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Unlock failed for descriptor2! %d", Result);
        goto cleanup;
    }

    LxtLogInfo("P: Waiting to acquire lock shared...");

    Result = flock(FileDescriptor2, LOCK_SH);

    if (Result < 0)
    {
        Result = errno;
        LxtLogError("Lock shared failed for descriptor2! %d", Result);
        goto cleanup;
    }

    LxtLogInfo("P: Sleeping 5 secs...");

    usleep(5 * 1000 * 1000);

    //
    // Force the child's file lock wait to be interrupted by a signal.
    //

    kill(ChildPid, SIGKILL);

    Result = LXT_RESULT_SUCCESS;

cleanup:

    if (FileDescriptor1 != -1)
    {
        close(FileDescriptor1);
    }

    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
    }

    if (DupedDescriptor != -1)
    {
        close(DupedDescriptor);
    }

    if (ChildPid == 0)
    {
        _exit(0);
    }

    return Result;
}
