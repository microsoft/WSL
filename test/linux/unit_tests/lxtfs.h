/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtfs.h

Abstract:

    This file contains common test functions for file system tests.

--*/

#include <sys/inotify.h>

#define FS_DRVFS_PREFIX "/mnt/c"
#define FS_DRVFS_DRIVE "C:"
#define FS_DRVFS_NAME "drvfs"
#define FS_WSLFS_NAME "wslfs"
#define FS_9P_NAME "9p"
#define FS_VIRTIOFS_NAME "virtiofs"
#define INOTIFY_TEST_EVENTS_BUF_SIZE 50
#define INOTIFY_TEST_FILE1_NAME_ONLY "a.txt"
#define INOTIFY_TEST_FILE2_NAME_ONLY "b.txt"
#define INOTIFY_TEST_FILE3_NAME_ONLY "c.txt"
#define INOTIFY_TEST_FILE1_SLINK_NAME_ONLY "as.txt"
#define INOTIFY_TEST_FILE1_HLINK_NAME_ONLY "ah.txt"
#define LXT_XATTR_CASE_SENSITIVE "system.wsl_case_sensitive"

//
// Flags for the utime test.
//

#define FS_UTIME_NT_PRECISION (0x1)
#define FS_UTIME_FAT (0x2)
#define FS_UTIME_NO_SYMLINKS (0x4)

//
// Flags for the getdents alignment test.
//

#define FS_TEST_GETDENTS64 (0x1)

//
// Flags for the timestamp test.
//

#define FS_TIMESTAMP_NOATIME (0x1)

//
// Flags for the delete tests.
//

#define FS_DELETE_DRVFS (0x1)

#define FS_IS_PLAN9_CACHED() ((g_LxtFsInfo.FsType == LxtFsTypePlan9) && (g_LxtFsInfo.Flags.Cached != 0))

typedef enum _LXT_FS_TYPE
{
    LxtFsTypeLxFs,
    LxtFsTypeWslFs,
    LxtFsTypeDrvFs,
    LxtFsTypePlan9,
    LxtFsTypeVirtioFs
} LXT_FS_TYPE,
    *PLXT_FS_TYPE;

typedef struct _LXT_FS_INFO
{
    LXT_FS_TYPE FsType;
    union
    {
        struct
        {
            int DrvFsBehavior : 1;
            int Cached : 1;
            int VirtIo : 1;
            int Dax : 1;
        };

        int AllFlags;
    } Flags;
} LXT_FS_INFO, *PLXT_FS_INFO;

int LxtFsCheckDrvFsMount(const char* Source, const char* Target, const char* Options, int ParentId, const char* MountRoot);

int LxtFsCreateTestDir(char* Directory);

int LxtFsDeleteCurrentWorkingDirectoryCommon(char* BaseDir, int Flags);

int LxtFsDeleteOpenFileCommon(char* BaseDir, int Flags);

int LxtFsDeleteLoopCommon(const char* BaseDir);

int LxtFsGetDentsAlignmentCommon(const char* BaseDir, int Flags);

int LxtFsGetFsInfo(const char* Path, PLXT_FS_INFO Info);

int LxtFsInotifyEpollCommon(char* BaseDir);

int LxtFsInotifyPosixUnlinkRenameCommon(char* BaseDir);

int LxtFsInotifyReadAndProcess(int Id, char* ReadBuf, int ReadBufSize, struct inotify_event** Events, int NumEventsIn, int* NumEventsOut, int IgnoreAttribModify);

int LxtFsInotifyUnmountBindCommon(char* BaseDir);

int LxtFsMountDrvFs(const char* Source, const char* Target, const char* Options);

int LxtFsRenameAtCommon(int DirFd1, int DirFd2);

int LxtFsRenameDirCommon(char* BaseDir);

void LxtFsTestCleanup(const char* TestDir, const char* DrvFsDir, bool UseDrvFs);

int LxtFsTestSetup(PLXT_ARGS Args, const char* TestDir, const char* DrvFsDir, bool UseDrvFs);

int LxtFsTimestampCommon(const char* BaseDir, int Flags);

int LxtFsUtimeBasicCommon(const char* BaseDir, int Flags);

void LxtFsUtimeCleanupTestFiles(const char* BaseDir);

int LxtFsUtimeCreateTestFiles(const char* BaseDir, int Flags);

int LxtFsWritevCommon(char* TestFile);

int LxtFsDirSeekCommon(const char* BaseDir);

extern LXT_FS_INFO g_LxtFsInfo;