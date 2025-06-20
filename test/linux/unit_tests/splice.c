/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    splice.c

Abstract:

    This file contains tests for the splice syscall.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#define LXT_NAME "Splice"

#define SPLICE_SYSCALL(_Fdin, _Offin, _Fdout, _Offout, _Size, _Flags) \
    syscall(SYS_splice, _Fdin, _Offin, _Fdout, _Offout, _Size, _Flags)

#define TEE_SYSCALL(_Fdin, _Fdout, _Size, _Flags) syscall(SYS_tee, _Fdin, _Fdout, _Size, _Flags)

#ifndef SPLICE_F_NONBLOCK
#define SPLICE_F_NONBLOCK 0x02
#endif

#define SPLICE_READ_PIPE_INDEX 0
#define SPLICE_WRITE_PIPE_INDEX 1

//
// The following defines a standardized file path and content for a file that
// can be used for splicing.
//

#define SPLICE_STD_FILE "/data/test/splice_test_std_file_1.txt"
#define SPLICE_STD_FILE_CONTENT "123456789Test"
#define SPLICE_STD_FILE_SIZE (unsigned long)(strlen(SPLICE_STD_FILE_CONTENT))

int SpliceOpenStandardFile(void);

int SpliceVariationBasicTests(PLXT_ARGS Args);

int SpliceVariationBlocking(PLXT_ARGS Args);

int SpliceVariationInvalidParameters(PLXT_ARGS Args);

int TeeVariationBasicTests(PLXT_ARGS Args);

int TeeVariationInvalidParameters(PLXT_ARGS Args);

//
// Globals.
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Splice - Invalid Parameter Test ", SpliceVariationInvalidParameters},
    {"Splice - Blocking Tests", SpliceVariationBlocking},
    {"Splice - Basic Usage Tests", SpliceVariationBasicTests},
    {"Tee - Invalid Parameter Test ", TeeVariationInvalidParameters},
    {"Tee - Basic Usage Test", TeeVariationBasicTests}};

int SpliceTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine is the main entry point for the procfs tests.

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

int SpliceOpenStandardFile(void)

/*++

Routine Description:

    This routine returns the file descriptor for the standard file which
    include uniform data for splicing.

Arguments:

    None.

Return Value:

    On success, returns open file descriptor; otherwise, -1.

--*/

{

    int Fd;
    off_t Offset;
    int Result;

    LxtCheckErrno(Fd = open(SPLICE_STD_FILE, (O_CREAT | O_RDWR), (S_IRUSR | S_IWUSR)));

    LxtCheckErrno(write(Fd, SPLICE_STD_FILE_CONTENT, SPLICE_STD_FILE_SIZE));

    //
    // Set the offset back to zero.
    //

    LxtCheckErrno(Offset = lseek(Fd, 0, SEEK_SET));

    //
    // Set the result.
    //

    Result = Fd;
    Fd = 0;

ErrorExit:
    if (Fd > 0)
    {
        close(Fd);
        unlink(SPLICE_STD_FILE);
    }

    return Result;
}

int SpliceVariationBasicTests(PLXT_ARGS Args)

/*++

Routine Description:

    This routine runs basic usage tests for splice, including splicing between
    two pipes and a pipe and a regular file, and tests the results for
    accurate splice sizes, content, and file offsets.

Arguments:

    Args - Supplies a pointer to variation arguments.

Return Value:

    On success, returns open file descriptor; otherwise, -1.


--*/

{

    char Buffer[4];
    LXT_PIPE DestinationPipe = {-1, -1};
    loff_t InitialOffset;
    loff_t Offset;
    char* PipeData;
    ssize_t PipeDataSize;
    int RegularFd;
    int Result;
    ssize_t SpliceSize;
    LXT_PIPE SourcePipe = {-1, -1};

    RegularFd = -1;
    PipeData = "1234";
    PipeDataSize = strlen(PipeData);
    LxtCheckResult(LxtCreatePipe(&DestinationPipe));
    LxtCheckResult(LxtCreatePipe(&SourcePipe));
    LxtCheckResult(RegularFd = SpliceOpenStandardFile());

    //
    // Set up read-end of pipes as non-blocking for empty tests.
    //

    LxtCheckErrno(fcntl(SourcePipe.Read, F_SETFL, O_NONBLOCK));
    LxtCheckErrno(fcntl(DestinationPipe.Read, F_SETFL, O_NONBLOCK));

    //
    // Perform basic tests between pipes.
    //

    LxtLogInfo("Basic Usage - Splicing between two pipes");
    LxtCheckErrno(write(SourcePipe.Write, PipeData, PipeDataSize));
    LxtCheckErrno(SpliceSize = SPLICE_SYSCALL(SourcePipe.Read, NULL, DestinationPipe.Write, NULL, PipeDataSize, SPLICE_F_NONBLOCK));

    LxtCheckEqual(SpliceSize, PipeDataSize, "%d");

    //
    // Check that the read pipe is now empty by attempting to read a single
    // byte.
    //

    LxtCheckErrnoFailure(read(SourcePipe.Read, Buffer, 1), EAGAIN);
    LxtCheckErrno(SpliceSize = read(DestinationPipe.Read, Buffer, PipeDataSize));

    LxtCheckEqual(SpliceSize, PipeDataSize, "%d");

    //
    // LX_TODO: Enable the following additional basic tests once splicing is
    // available for VolFs file types.
    //

    /*
    //
    // Perform test from a regular file to a pipe.
    //

    InitialOffset = 4;
    Offset = InitialOffset;
    LxtLogInfo("Basic Usage - Splicing from a regular file to a pipe");
    LxtCheckErrno(SpliceSize = SPLICE_SYSCALL(RegularFd,
                                              &Offset,
                                              DestinationPipe.Write,
                                              NULL,
                                              4,
                                              SPLICE_F_NONBLOCK));

    LxtCheckEqual(SpliceSize, 4, "%d");
    LxtCheckErrno(SpliceSize = read(DestinationPipe.Read, Buffer, 4));
    LxtCheckEqual(SpliceSize, 4, "%d");
    LxtCheckEqual(Offset - InitialOffset, 4, "%d");
    LxtCheckEqual(Buffer[0],
                  SPLICE_STD_FILE_CONTENT[InitialOffset],
                  "%c");

    //
    // Ensure that offset remains unchanged on the file.
    //

    LxtCheckErrno(SpliceSize = read(RegularFd, Buffer, 4));
    LxtCheckEqual(SpliceSize, 4, "%d");
    LxtCheckEqual(Buffer[0],
                  SPLICE_STD_FILE_CONTENT[0],
                  "%c");

    //
    // Reset the file's offset for the next test.
    //

    LxtCheckErrno(lseek(RegularFd, 0, SEEK_SET));

    //
    // Perform test from a pipe to a regular file.
    //

    InitialOffset = 1;
    Offset = InitialOffset;
    LxtLogInfo("Basic Usage - Splicing from a pipe to a regular file");
    LxtCheckErrno(write(SourcePipe.Write, PipeData, PipeDataSize));
    LxtCheckErrno(SpliceSize = SPLICE_SYSCALL(SourcePipe.Read,
                                              NULL,
                                              RegularFd,
                                              &Offset,
                                              PipeDataSize,
                                              SPLICE_F_NONBLOCK));

    LxtCheckEqual(SpliceSize, PipeDataSize, "%d");
    LxtCheckEqual(Offset - InitialOffset, PipeDataSize, "%d");

    //
    // Ensure that the regular file's internal offset is unchanged and that the
    // the pipe data was properly splice into the file with the offset.
    //

    LxtCheckErrno(SpliceSize = read(RegularFd, Buffer, 2));
    LxtCheckEqual(SpliceSize, 2, "%d");
    LxtCheckEqual(Buffer[0],
                  SPLICE_STD_FILE_CONTENT[0],
                  "%c");

    LxtCheckEqual(Buffer[1], PipeData[0], "%c");
    */

ErrorExit:
    LxtClosePipe(&DestinationPipe);
    LxtClosePipe(&SourcePipe);
    if (RegularFd > 0)
    {
        close(RegularFd);
    }

    return Result;
}

int SpliceVariationBlocking(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the splice syscall with the splice-specific non-blocking
    flag with pipes that have opposite internal blocking settings and checks for
    proper behavior.

Arguments:

    Args - Supplies a pointer to variation arguments.

Return Value:

    On success, returns open file descriptor; otherwise, -1.


--*/

{

    pid_t ChildPid;
    LXT_PIPE DestinationPipe = {-1, -1};
    int Result;
    LXT_PIPE SourcePipe = {-1, -1};
    int WaitPidResult;
    int WaitPidStatus;

    LxtCheckResult(LxtCreatePipe(&DestinationPipe));
    LxtCheckResult(LxtCreatePipe(&SourcePipe));

    //
    // Verify that a pipe which is automatically set to have blocking I/O
    // semantics, does not block when the splice call is supplied with the
    // splice-specific non-blocking flag.
    //

    LxtLogInfo("Blocking - Non-blocking splice with blocking pipes");
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // By default, the created pipes are blocking. The splice non-blocking
        // flag should override this behavior.
        //

        Result = SPLICE_SYSCALL(SourcePipe.Read, NULL, DestinationPipe.Write, NULL, 1, SPLICE_F_NONBLOCK);

        if (Result >= 0)
        {
            LxtLogError("Non-blocking splice syscall succeeded");
            _exit(1);
        }

        if (errno != EAGAIN)
        {
            LxtLogError("Non-blocking splice syscall returned with error %s", strerror(errno));

            _exit(1);
        }

        _exit(0);
    }

    LxtCheckResult(WaitPidStatus = LxtWaitPidPollOptions(ChildPid, 0, 0, 2));

    //
    // Verify that a pipe that is set to non-blocking will block when the
    // splice-specific non-blocking flag is not passed to splice.
    //

    LxtLogInfo("Blocking - Blocking splice with non-blocking pipe");
    LxtCheckErrno(fcntl(SourcePipe.Read, F_SETFL, O_NONBLOCK));
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        Result = SPLICE_SYSCALL(SourcePipe.Read, NULL, DestinationPipe.Write, NULL, 1, 0);

        _exit(0);
    }

    sleep(2);
    LxtCheckErrno(WaitPidResult = waitpid(ChildPid, &WaitPidStatus, WNOHANG));

    //
    // If the child is not alive, it did not block as expected.
    //

    LxtCheckEqual(WaitPidResult, 0, "%d");
    kill(ChildPid, SIGKILL);
    Result = 0;
ErrorExit:
    LxtClosePipe(&DestinationPipe);
    LxtClosePipe(&SourcePipe);
    return Result;
}

int SpliceVariationInvalidParameters(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the splice syscall errors are properly set when invalid
    parameters are passed to it.

Arguments:

    Args - Supplies a pointer to variation arguments.

Return Value:

    On success, returns open file descriptor; otherwise, -1.

--*/
{

    LXT_PIPE DestinationPipe = {-1, -1};
    loff_t ReadOffset;
    int Result;
    LXT_PIPE SourcePipe = {-1, -1};
    int StandardFd;

    StandardFd = 0;
    LxtCheckErrno(LxtCreatePipe(&SourcePipe));
    LxtCheckErrno(LxtCreatePipe(&DestinationPipe));
    LxtCheckResult(StandardFd = SpliceOpenStandardFile());

    //
    // Put some random data into the source pipe and set the offset.
    //

    LxtCheckErrno(write(SourcePipe.Write, "1234", 4));
    ReadOffset = 2;

    //
    // Check that a call with invalid parameters, but a splice size of zero will
    // succeed.
    //

    LxtLogInfo("Invalid Params - Passing invalid parameters with splice size of zero");

    LxtCheckErrno(SPLICE_SYSCALL(SourcePipe.Read, &ReadOffset, DestinationPipe.Write, NULL, 0, 0));

    LxtCheckErrno(SPLICE_SYSCALL(StandardFd, NULL, StandardFd, NULL, 0, 0));
    LxtCheckErrno(SPLICE_SYSCALL(-1, NULL, -1, NULL, 0, 0));

    //
    // Check that invalid flags do not cause any errors.
    //

    LxtCheckErrno(SPLICE_SYSCALL(SourcePipe.Read, NULL, DestinationPipe.Write, NULL, 4, 0xF0));

    //
    // Check the error result when a pipe is given a non-null offset.
    //

    LxtLogInfo("Invalid Params - Passing non-null offset with pipe fd");
    LxtCheckErrnoFailure(SPLICE_SYSCALL(SourcePipe.Read, &ReadOffset, DestinationPipe.Write, NULL, 1, 0), ESPIPE);

    LxtCheckErrnoFailure(SPLICE_SYSCALL(SourcePipe.Read, NULL, DestinationPipe.Write, &ReadOffset, 1, 0), ESPIPE);

    //
    // Ensure that the correct error is returned when splice takes two
    // parameters that are not pipes.
    //

    LxtLogInfo("Invalid Params - Passing two non-pipe fd's to splice");
    LxtCheckErrnoFailure(SPLICE_SYSCALL(StandardFd, NULL, StandardFd, NULL, 1, 0), EINVAL);

    //
    // Validate errors returned wrong read\write pipe
    //

    //
    // LX_TODO: Here and in the tee syscall variation, the atomic property of
    // tee/splice must be implemented before these tests are enabled.
    //

    /*
        LxtLogInfo("Invalid Params - Invalid splice read\write pipe 1");
        LxtCheckErrnoFailure(SPLICE_SYSCALL(SourcePipe.Read,
                                            NULL,
                                            SourcePipe.Read,
                                            NULL,
                                            200,
                                            0),
                             EBADF);

        LxtLogInfo("Invalid Params - Invalid splice read\write pipe 2");
        LxtCheckErrnoFailure(SPLICE_SYSCALL(SourcePipe.Write,
                                            NULL,
                                            SourcePipe.Read,
                                            NULL,
                                            200,
                                            0),
                             EBADF);

        LxtLogInfo("Invalid Params - Invalid splice read\write pipe 3");
        LxtCheckErrnoFailure(SPLICE_SYSCALL(SourcePipe.Read,
                                            NULL,
                                            DestinationPipe.Read,
                                            NULL,
                                            200,
                                            0),
                             EBADF);
    */

ErrorExit:
    LxtClosePipe(&SourcePipe);
    LxtClosePipe(&DestinationPipe);
    if (StandardFd > 0)
    {
        close(StandardFd);
    }

    return Result;
}

int TeeVariationBasicTests(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[16];
    LXT_PIPE DestinationPipe = {-1, -1};
    char* PipeData;
    ssize_t PipeDataSize;
    ssize_t ReadSize;
    int Result;
    ssize_t SpliceSize;
    LXT_PIPE SourcePipe = {-1, -1};

    PipeData = "1234";
    PipeDataSize = strlen(PipeData);
    LxtCheckResult(LxtCreatePipe(&DestinationPipe));
    LxtCheckResult(LxtCreatePipe(&SourcePipe));

    //
    // Set up read-end of pipes as non-blocking for empty tests.
    //

    LxtCheckErrno(fcntl(SourcePipe.Read, F_SETFL, O_NONBLOCK));
    LxtCheckErrno(fcntl(DestinationPipe.Read, F_SETFL, O_NONBLOCK));

    //
    // Perform basic tests between pipes.
    //

    LxtLogInfo("Basic Usage - Tee");
    LxtCheckErrno(write(SourcePipe.Write, PipeData, PipeDataSize));
    LxtCheckErrno(SpliceSize = TEE_SYSCALL(SourcePipe.Read, DestinationPipe.Write, PipeDataSize, SPLICE_F_NONBLOCK));

    LxtCheckEqual(SpliceSize, PipeDataSize, "%d");

    //
    // Check that the read pipe still has the same data as before.
    //

    LxtCheckErrno(ReadSize = read(SourcePipe.Read, Buffer, PipeDataSize));
    LxtCheckEqual(ReadSize, PipeDataSize, "%d");
    Buffer[ReadSize] = '\0';
    LxtCheckStringEqual(Buffer, PipeData);

    //
    // Check that the destination pipe has the correct data after the tee.
    //

    LxtCheckErrno(ReadSize = read(DestinationPipe.Read, Buffer, PipeDataSize));
    LxtCheckEqual(ReadSize, PipeDataSize, "%d");
    Buffer[ReadSize] = '\0';
    LxtCheckStringEqual(Buffer, PipeData);

ErrorExit:
    LxtClosePipe(&SourcePipe);
    LxtClosePipe(&DestinationPipe);
    return Result;
}

int TeeVariationInvalidParameters(PLXT_ARGS Args)

/*++
--*/

{

    LXT_PIPE DestinationPipe = {-1, -1};
    int Result;
    LXT_PIPE SourcePipe = {-1, -1};
    int StandardFd;

    StandardFd = 0;
    LxtCheckErrno(LxtCreatePipe(&SourcePipe));
    LxtCheckErrno(LxtCreatePipe(&DestinationPipe));
    LxtCheckResult(StandardFd = SpliceOpenStandardFile());

    //
    // Put some random data into the source pipe and set the offset.
    //

    LxtCheckErrno(write(SourcePipe.Write, "1234", 4));

    //
    // Check that a call with invalid parameters, but a size of zero will
    // succeed.
    //

    LxtLogInfo("Invalid Params - Passing invalid parameters to tee with size of zero");

    LxtCheckErrno(TEE_SYSCALL(StandardFd, DestinationPipe.Write, 0, 0));
    LxtCheckErrno(TEE_SYSCALL(SourcePipe.Read, StandardFd, 0, 0));
    LxtCheckErrno(TEE_SYSCALL(StandardFd, StandardFd, 0, 0));

    //
    // Check that invalid flags do not cause any errors.
    //

    LxtCheckErrno(TEE_SYSCALL(SourcePipe.Read, DestinationPipe.Write, 4, 0xF0));
    //
    // Validate the errors returned with invalid parameters and non-zero splice
    // sizes.
    //

    LxtLogInfo("Invalid Params - Passing a non-pipe to a tee syscall");
    LxtCheckErrnoFailure(TEE_SYSCALL(StandardFd, DestinationPipe.Write, 1, 0), EINVAL);

    LxtCheckErrnoFailure(TEE_SYSCALL(SourcePipe.Read, StandardFd, 1, 0), EINVAL);

    //
    // Validate errors returned sending to the wrong read\write pipe
    //

ErrorExit:
    LxtClosePipe(&SourcePipe);
    LxtClosePipe(&DestinationPipe);
    if (StandardFd > 0)
    {
        close(StandardFd);
    }

    return Result;
}
