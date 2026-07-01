/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    drvfs.h

Abstract:

    This file contains DrvFs function declarations.

--*/

#pragma once
#include <optional>
#include "WslDistributionConfig.h"

#define DRVFS_FS_TYPE "drvfs"
#define MOUNT_DRVFS_NAME "mount.drvfs"

//
// Hidden well-known directory under which aggregate virtio-fs devices are
// mounted (one mount per device tag, at "<dir>/<tag>"). The per-share bind
// mounts reference children of these device mounts.
//
#define VIRTIOFS_DEVICE_DIR "/run/wsl/virtiofs-dev"

int MountDrvfs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode = nullptr);

int MountDrvfsEntry(int Argc, char* Argv[]);

int MountPlan9Share(const char* Source, const char* Target, const char* Options, bool Admin, int* ExitCode = nullptr);

int MountPlan9(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode);

int MountVirtioFs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode = nullptr);

int MountVirtioFsAggregateChild(const char* Tag, const char* Subname, const char* Target, const char* MountOptions, int* ExitCode = nullptr);

int RemountVirtioFs(const char* Subname, const char* Target, const char* Options, bool Admin);

std::string QueryVirtiofsMountSource(const char* Tag);
