/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    fstab.c

Abstract:

    This file contains tests for fstab mounting.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <libmount/libmount.h>

#define LXT_NAME "fstab"

LXT_VARIATION_HANDLER FsTabTestMount;

static const LXT_VARIATION g_LxtVariations[] = {{"FsTab - DrvFs mounted through fstab", FsTabTestMount}};

int FstabTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the wslpath tests.

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

int FsTabTestMount(PLXT_ARGS Args)

/*++

Description:

    This routine tests whether fstab mounting was performed correctly.

    N.B. This test should be run after changing the /etc/fstab file and
         restarting the instance.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct libmnt_fs* FileSystem;
    bool Found;
    const char* FsType;
    struct libmnt_iter* Iterator;
    const char* Options;
    int Result;
    const char* Source;
    struct libmnt_table* Table;

    Iterator = NULL;
    Table = mnt_new_table_from_file("/proc/self/mountinfo");
    LxtCheckNotEqual(Table, NULL, "%p");
    Iterator = mnt_new_iter(MNT_ITER_FORWARD);
    LxtCheckNotEqual(Iterator, NULL, "%p");
    Found = false;
    while (mnt_table_next_fs(Table, Iterator, &FileSystem) == 0)
    {
        FsType = mnt_fs_get_fstype(FileSystem);
        Options = mnt_fs_get_fs_options(FileSystem);

        //
        // Check that there is only one mount for C: (or any variation therefore, like C:\ or c:),
        // and that its mount uses the exact options specified in fstab.
        //

        if (strcmp(FsType, "9p") == 0)
        {
            if (strcasestr(Options, "aname=drvfs;path=C:") != NULL)
            {
                LxtCheckTrue(!Found);
                LxtCheckNotEqual(strstr(Options, "aname=drvfs;path=C:\\;metadata;"), NULL, "%p");
                Found = true;
            }
        }
        else if (strcmp(FsType, "drvfs") == 0)
        {
            Source = mnt_fs_get_source(FileSystem);
            if (strcasestr(Source, "C:") == Source)
            {
                LxtCheckTrue(!Found);
                LxtCheckStringEqual(Source, "C:\\");
                LxtCheckStringEqual(Options, "rw,metadata,case=off");
                Found = true;
            }
        }
        else if (strcmp(FsType, "virtiofs") == 0)
        {
            Source = mnt_fs_get_source(FileSystem);
            if (strcasestr(Source, "drvfsaC") == Source)
            {
                LxtCheckTrue(!Found);
                LxtCheckStringEqual(Options, "rw");
                Found = true;
            }
        }
    }

    LxtCheckTrue(Found);

ErrorExit:
    if (Iterator != NULL)
    {
        mnt_free_iter(Iterator);
    }

    if (Table != NULL)
    {
        mnt_free_table(Table);
    }

    return Result;
}