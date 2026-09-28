/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    user.c

Abstract:

    This file is the source for the user management.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <pwd.h>

#define LXT_NAME "user"

int ValidateUserTest(char* Username, uid_t Uid, uid_t Gid);

int UserTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    uid_t Gid;
    int Result = LXT_RESULT_FAILURE;
    uid_t Uid;
    char* Username;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));

    if (Argc < 4)
    {
        LxtLogError("User test requires three arguments: username, uid, gid");
        goto ErrorExit;
    }

    Username = Argv[1];
    Uid = atoi(Argv[2]);
    Gid = atoi(Argv[3]);
    LxtCheckResult(ValidateUserTest(Username, Uid, Gid));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int ValidateUserTest(char* Username, uid_t Uid, uid_t Gid)

/*++
--*/

{

    struct passwd* PasswordEntry;
    uid_t RealGid;
    uid_t RealUid;
    int Result = LXT_RESULT_FAILURE;

    RealUid = getuid();
    if (Uid != RealUid)
    {
        LxtLogError("Uid %u does not match RealUid %u", Uid, RealUid);
        goto ErrorExit;
    }

    RealGid = getgid();
    if (Gid != RealGid)
    {
        LxtLogError("Gid %u does not match RealGid %u", Gid, RealGid);
        goto ErrorExit;
    }

    //
    // Compare passed-in values to the values stored in the password entry file.
    //

    PasswordEntry = getpwnam(Username);
    if (PasswordEntry == NULL)
    {
        LxtLogError("getpwnam %s failed", Username);
        goto ErrorExit;
    }

    if (Uid != PasswordEntry->pw_uid)
    {
        LxtLogError("Uid %u does not match PasswordEntry->pw_uid %u", Uid, PasswordEntry->pw_uid);

        goto ErrorExit;
    }

    if (Gid != PasswordEntry->pw_gid)
    {
        LxtLogError("Gid %u does not match PasswordEntry->pw_gid %u", Gid, PasswordEntry->pw_gid);

        goto ErrorExit;
    }

    if (strstr(PasswordEntry->pw_dir, Username) == NULL)
    {
        LxtLogError("Home path %s does not contain Username %s", PasswordEntry->pw_dir, Username);

        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;
    LxtLogPassed("Username %s, Uid %u, Gid %u successfully validated!", Username, Uid, Gid);

ErrorExit:
    return Result;
}
