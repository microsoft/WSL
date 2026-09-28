/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Keymgmt.c

Abstract:

    This file is a keymgmt test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <linux/keyctl.h>

#define LXT_NAME "Keymgmt"

#define LXT_KEYMGMT_DESCRIBE_LENGTH 128

#define LxtKeyCtl(_Cmd, _Arg2, _Arg3, _Arg4, _Arg5) syscall(SYS_keyctl, (_Cmd), (_Arg2), (_Arg3), (_Arg4), (_Arg5))
#define LxtAdd_Key(_Type, _Desc, _Payload, _Length, _KeyRing) \
    syscall(SYS_add_key, (_Type), (_Desc), (_Payload), (_Length), (_KeyRing))
#define LxtRequest_Key(_Type, _Desc, _Info, _KeyRing) syscall(SYS_request_key, (_Type), (_Desc), (_Info), (_KeyRing))

#define KEY_POS_VIEW 0x01000000
#define KEY_POS_READ 0x02000000
#define KEY_POS_WRITE 0x04000000
#define KEY_POS_SEARCH 0x08000000
#define KEY_POS_LINK 0x10000000
#define KEY_POS_SETATTR 0x20000000
#define KEY_POS_ALL 0x3f000000
#define KEY_USR_VIEW 0x00010000
#define KEY_USR_READ 0x00020000
#define KEY_USR_WRITE 0x00040000
#define KEY_USR_SEARCH 0x00080000
#define KEY_USR_LINK 0x00100000
#define KEY_USR_SETATTR 0x00200000
#define KEY_USR_ALL 0x003f0000
#define KEY_GRP_VIEW 0x00000100
#define KEY_GRP_READ 0x00000200
#define KEY_GRP_WRITE 0x00000400
#define KEY_GRP_SEARCH 0x00000800
#define KEY_GRP_LINK 0x00001000
#define KEY_GRP_SETATTR 0x00002000
#define KEY_GRP_ALL 0x00003f00
#define KEY_OTH_VIEW 0x00000001
#define KEY_OTH_READ 0x00000002
#define KEY_OTH_WRITE 0x00000004
#define KEY_OTH_SEARCH 0x00000008
#define KEY_OTH_LINK 0x00000010
#define KEY_OTH_SETATTR 0x00000020
#define KEY_OTH_ALL 0x0000003f

#define KEY_INVALID -1

#define LXT_KEYMGMT_ALLPERMS (KEY_POS_ALL | KEY_USR_ALL | KEY_GRP_ALL | KEY_OTH_ALL)

#define LXT_KEYMGMT_DEFAULTPERMS (0x3f130000)
#define LXT_KEYMGMT_DEFAULTPERMS_STRING "3f130000"

#define LXT_KEYMGMT_NEWPERMS (0x3f3f0000)
#define LXT_KEYMGMT_NEWPERMS_STRING "3f3f0000"

#define LXT_KEYMGMT_SESIONKEYRING_NAME "sessionkeyring"
#define LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS "keyring;0;0;" LXT_KEYMGMT_DEFAULTPERMS_STRING ";" LXT_KEYMGMT_SESIONKEYRING_NAME
#define LXT_KEYMGMT_SESIONKEYRING_NEWPERMS "keyring;0;0;" LXT_KEYMGMT_NEWPERMS_STRING ";" LXT_KEYMGMT_SESIONKEYRING_NAME

#define LXT_KEYMGMT_SESIONKEYRING2_NAME "sessionkeyring2"
#define LXT_KEYMGMT_SESIONKEYRING2_DEFAULTPERMS "keyring;0;0;" LXT_KEYMGMT_DEFAULTPERMS_STRING ";" LXT_KEYMGMT_SESIONKEYRING2_NAME

#define LX_KEYMGMT_LONG_NAME_SIZE (4096 + 1)

LXT_VARIATION_HANDLER KeymgmtSessionKeyringAssociation;

LXT_VARIATION_HANDLER KeymgmtJoinSessionKeyring;

LXT_VARIATION_HANDLER KeymgmtDescribe;

LXT_VARIATION_HANDLER KeymgmtSetPerm;

//
// Global constants
//

//
// TODO_LX: Enable KeymgmtSessionKeyringAssociation when supported.
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Keymgmt - KEYCTL_JOIN_SESSION_KEYRING", KeymgmtJoinSessionKeyring},
    {"Keymgmt - KEYCTL_DESCRIBE", KeymgmtDescribe},
    {"Keymgmt - KEYCTL_SETPERM", KeymgmtSetPerm},
    /*{"Keymgmt session keyring association", KeymgmtSessionKeyringAssociation}*/};

int KeymgmtTestEntry(int Argc, char* Argv[])

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

void* KeymgmtSessionKeyringAssociationThread(void* Args)

{

    char KeyBufferNew[LXT_KEYMGMT_DESCRIBE_LENGTH];
    int32_t* KeySerial;
    int32_t KeySerialNew;
    int KeyType;
    int Result;

    KeyType = KEY_SPEC_SESSION_KEYRING;

    //
    // Check that the session keyring id didn't change.
    //

    KeySerial = Args;
    LxtCheckErrno(KeySerialNew = LxtKeyCtl(KEYCTL_GET_KEYRING_ID, KeyType, 0, 0, 0));
    LxtCheckEqual(*KeySerial, KeySerialNew, "%d");
    LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerialNew, LXT_KEYMGMT_ALLPERMS, 0, 0));
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerialNew, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringNotEqual(LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS, KeyBufferNew);

ErrorExit:
    pthread_exit(&Result);
}

int KeymgmtSessionKeyringAssociation(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid;
    char KeyBuffer[LXT_KEYMGMT_DESCRIBE_LENGTH];
    char KeyBufferNew[LXT_KEYMGMT_DESCRIBE_LENGTH];
    int32_t KeySerial;
    int32_t KeySerialNew;
    int32_t KeySerialOriginal;
    int KeyType;
    int Result;
    pthread_t Thread = {0};

    ChildPid = -1;
    KeyType = KEY_SPEC_SESSION_KEYRING;

    //
    // This test checks to see where the session keyring is associated. The
    // documentation is unclear if it is the threadgroup, thread, or user
    // namespace. The test below validates that the session keyring is
    // associated to the threadgroup and inherited across fork.
    //

    //
    // Get the current session keyring and check that is changes when a new
    // session keyring is created.
    //

    KeySerialOriginal = LxtKeyCtl(KEYCTL_GET_KEYRING_ID, KeyType, 0, 0, 0);
    if (KeySerialOriginal == -1)
    {
        KeySerialOriginal = 0;
    }

    LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING_NAME, 0, 0, 0));
    LxtCheckNotEqual(KeySerialOriginal, KeySerial, "%d");
    LxtLogInfo("Key %d", KeySerial);
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0));
    LxtCheckStringEqual(KeyBuffer, LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS);

    LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_ALLPERMS, 0, 0));
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringNotEqual(KeyBuffer, KeyBufferNew);

    LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_DEFAULTPERMS, 0, 0));
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringEqual(KeyBuffer, KeyBufferNew);

    //
    // Create a child process and thread, checking that the session keyring id
    // continues to be associated.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerialNew = LxtKeyCtl(KEYCTL_GET_KEYRING_ID, KeyType, 0, 0, 0));
        LxtCheckEqual(KeySerial, KeySerialNew, "%d");
        LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_ALLPERMS, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
        LxtCheckStringNotEqual(KeyBuffer, KeyBufferNew);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // The changes from the child threadgroup should reflect into the parent.
    //

    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringNotEqual(KeyBuffer, KeyBufferNew);
    LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_DEFAULTPERMS, 0, 0));
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringEqual(KeyBuffer, KeyBufferNew);

    //
    // Repeat the scenario with a thread.
    //

    LxtCheckErrno(pthread_create(&Thread, NULL, KeymgmtSessionKeyringAssociationThread, &KeySerial));

    pthread_join(Thread, NULL);
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringNotEqual(KeyBuffer, KeyBufferNew);
    LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_DEFAULTPERMS, 0, 0));
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringEqual(KeyBuffer, KeyBufferNew);

    //
    // Create a user namespace and check that the session keyring id continues
    // to be associated.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerialNew = LxtKeyCtl(KEYCTL_GET_KEYRING_ID, KeyType, 0, 0, 0));
        LxtCheckEqual(KeySerial, KeySerialNew, "%d");
        LxtCheckErrno(unshare(CLONE_NEWUSER));
        LxtCheckErrno(KeySerialNew = LxtKeyCtl(KEYCTL_GET_KEYRING_ID, KeyType, 0, 0, 0));
        LxtCheckEqual(KeySerial, KeySerialNew, "%d");
        LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_ALLPERMS, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
        LxtCheckStringNotEqual(KeyBuffer, KeyBufferNew);
        _exit(LXT_RESULT_SUCCESS);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));

    //
    // The changes from the child threadgroup should reflect into the parent.
    //

    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringNotEqual(KeyBuffer, KeyBufferNew);
    LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_DEFAULTPERMS, 0, 0));
    LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBufferNew, sizeof(KeyBufferNew), 0));
    LxtCheckStringEqual(KeyBuffer, KeyBufferNew);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int KeymgmtJoinSessionKeyring(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid = -1;
    int Index;
    char* LongName = NULL;
    char KeyBuffer[LXT_KEYMGMT_DESCRIBE_LENGTH];
    int32_t KeySerial;
    int32_t KeySerial2;
    LXT_PIPE Pipe = {-1, -1};
    int Result;
    char* ValidNames[] = {"1", "a", "1a", ";", "name with a space ", "name with a tab\t", "name with a new line\n"};

    //
    // This test checks how KEYCTL_JOIN_SESSION_KEYRING handles keyrings.
    //

    //
    // Check for valid names.
    //

    for (Index = 0; Index < LXT_COUNT_OF(ValidNames); ++Index)
    {
        LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, ValidNames[Index], 0, 0, 0));
    }

    //
    // Check for a really long name.
    //

    LongName = LxtAlloc(LX_KEYMGMT_LONG_NAME_SIZE);
    if (LongName == 0)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    memset(LongName, 'a', LX_KEYMGMT_LONG_NAME_SIZE);
    LongName[LX_KEYMGMT_LONG_NAME_SIZE - 1] = 0;
    LxtCheckErrnoFailure(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LongName, 0, 0, 0), EINVAL);
    LongName[LX_KEYMGMT_LONG_NAME_SIZE - 2] = 0;
    LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LongName, 0, 0, 0));

    //
    // TODO_LX: Add support for NULL name when supported.
    //

    //
    // Invalid parameters.
    //

    LxtCheckErrnoFailure(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, (void*)0x1, 0, 0, 0), EFAULT);

    //
    // Check for lifetime.
    //

    LxtCheckResult(LxtCreatePipe(&Pipe));
    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING_NAME, 0, 0, 0));
        LxtCheckErrno(write(Pipe.Write, &KeySerial, sizeof(KeySerial)));
        _exit(0);
    }

    LxtCheckErrno(read(Pipe.Read, &KeySerial, sizeof(KeySerial)));
    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    sleep(1);
    LxtCheckErrnoFailure(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0), ENOKEY);

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING_NAME, 0, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0));
        LxtCheckStringEqual(KeyBuffer, LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS);
        LxtCheckErrno(KeySerial2 = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING2_NAME, 0, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial2, KeyBuffer, sizeof(KeyBuffer), 0));
        LxtCheckStringEqual(KeyBuffer, LXT_KEYMGMT_SESIONKEYRING2_DEFAULTPERMS);
        sleep(1);
        LxtCheckErrnoFailure(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0), ENOKEY);
        _exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    if (LongName != NULL)
    {
        LxtFree(LongName);
    }

    LxtClosePipe(&Pipe);
    return Result;
}

int KeymgmtDescribe(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid = -1;
    int BytesRequired;
    char KeyBuffer[LXT_KEYMGMT_DESCRIBE_LENGTH];
    int32_t KeySerial;
    int Result;

    //
    // This test checks how KEYCTL_DESCRIBE handles parameters.
    //

    //
    // Check for the default values.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING_NAME, 0, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0));
        LxtCheckStringEqual(KeyBuffer, LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS);
        LxtCheckErrno(BytesRequired = LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, NULL, 0, 0));
        LxtCheckEqual(BytesRequired, sizeof(LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS), "%d");
        LxtCheckErrno(BytesRequired = LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, (void*)0x1, 1, 0));
        LxtCheckEqual(BytesRequired, sizeof(LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS), "%d");
        _exit(0);
    }

    LxtWaitPidPoll(ChildPid, 0);

    //
    // TODO_LX: Add support for NULL name when supported.
    //

    //
    // Invalid parameters.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING_NAME, 0, 0, 0));
        LxtCheckErrnoFailure(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, (void*)0x1, sizeof(KeyBuffer), 0), EFAULT);
        LxtCheckErrnoFailure(LxtKeyCtl(KEYCTL_DESCRIBE, KEY_INVALID, KeyBuffer, sizeof(KeyBuffer), 0), ENOKEY);
        _exit(0);
    }

    LxtWaitPidPoll(ChildPid, 0);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int KeymgmtSetPerm(PLXT_ARGS Args)

/*++
--*/

{

    pid_t ChildPid = -1;
    int BytesRequired;
    char KeyBuffer[LXT_KEYMGMT_DESCRIBE_LENGTH];
    int32_t KeySerial;
    int Result;

    //
    // This test checks how KEYCTL_SETPERM handles parameters.
    //

    //
    // Check for the default values.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING_NAME, 0, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0));
        LxtCheckStringEqual(KeyBuffer, LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS);
        LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_DEFAULTPERMS, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0));
        LxtCheckStringEqual(KeyBuffer, LXT_KEYMGMT_SESIONKEYRING_DEFAULTPERMS);
        LxtCheckErrno(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, LXT_KEYMGMT_NEWPERMS, 0, 0));
        LxtCheckErrno(LxtKeyCtl(KEYCTL_DESCRIBE, KeySerial, KeyBuffer, sizeof(KeyBuffer), 0));
        LxtCheckStringEqual(KeyBuffer, LXT_KEYMGMT_SESIONKEYRING_NEWPERMS);
        _exit(0);
    }

    LxtWaitPidPoll(ChildPid, 0);

    //
    // TODO_LX: Add support for NULL name when supported.
    //

    //
    // Invalid parameters.
    //

    LxtCheckResult(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrno(KeySerial = LxtKeyCtl(KEYCTL_JOIN_SESSION_KEYRING, LXT_KEYMGMT_SESIONKEYRING_NAME, 0, 0, 0));
        LxtCheckErrnoFailure(LxtKeyCtl(KEYCTL_SETPERM, 0, LXT_KEYMGMT_DEFAULTPERMS, 0, 0), EINVAL);
        LxtCheckErrnoFailure(LxtKeyCtl(KEYCTL_SETPERM, KeySerial, -1, 0, 0), EINVAL);
        _exit(0);
    }

    LxtWaitPidPoll(ChildPid, 0);

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}
