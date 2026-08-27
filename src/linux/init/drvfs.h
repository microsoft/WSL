/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    drvfs.h

Abstract:

    This file contains DrvFs function declarations.

--*/

#pragma once

#define VIRTIOFS_MOUNT_DIR "/run/wsl/virtiofs-mounts"
#include <optional>
#include <string_view>
#include "WslDistributionConfig.h"

#define DRVFS_FS_TYPE "drvfs"
#define MOUNT_DRVFS_NAME "mount.drvfs"

struct VirtioFsMountRoot
{
    std::string_view ChildName;
    std::string_view SubPath;
};

std::optional<VirtioFsMountRoot> ParseAggregateVirtioFsMountRoot(std::string_view Tag, std::string_view Root);

int MountDrvfs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode = nullptr);

int MountDrvfsEntry(int Argc, char* Argv[]);

int MountPlan9Share(const char* Source, const char* Target, const char* Options, bool Admin, int* ExitCode = nullptr);

int MountPlan9(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode);

int MountVirtioFs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode = nullptr);

int RemountVirtioFs(const char* Tag, const char* Target, const char* Options, bool Admin, std::string_view SubPath = "/");

std::string QueryVirtiofsMountSource(const char* Tag, const char* Root = nullptr);
