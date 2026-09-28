// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#define MOUNT_INFO_FILE_NAME "/mountinfo"
#define MOUNT_INFO_FILE "/proc/self" MOUNT_INFO_FILE_NAME

// Represents the parsed data from a line in the /proc/self/mountinfo file.
typedef struct _MOUNT_ENTRY
{
    int Id;
    int ParentId;
    dev_t Device;
    char* Root;
    char* MountPoint;
    char* MountOptions;
    char* FileSystemType;
    char* Source;
    char* SuperOptions;
} MOUNT_ENTRY, *PMOUNT_ENTRY;

// Represents an enumeration of entries in the /proc/self/mountinfo file.
typedef struct _MOUNT_ENUM
{
    FILE* MountInfo;
    char* Line;
    size_t LineLength;
    MOUNT_ENTRY Current;
} MOUNT_ENUM, *PMOUNT_ENUM;

int MountEnumCreate(PMOUNT_ENUM mountEnum);

int MountEnumCreateEx(PMOUNT_ENUM mountEnum, const char* mountInfoFile);

void MountEnumFree(PMOUNT_ENUM mountEnum);

int MountEnumNext(PMOUNT_ENUM mountEnum);

int MountParseMountInfoLine(char* line, PMOUNT_ENTRY entry);
