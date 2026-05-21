/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    drvfs.h

Abstract:

    This file contains DrvFs function declarations.

--*/

#pragma once
#include <optional>
#include <string>
#include "WslDistributionConfig.h"

#define DRVFS_FS_TYPE "drvfs"
#define MOUNT_DRVFS_NAME "mount.drvfs"

//
// DrvFs transport selector used by the per-mount transport override.
//
// Default     - Use the global default selected by feature flags.
// Plan9       - Force plan9 over hvsocket.
// Virtio9p    - Force plan9 over virtio (virtio-9p).
// Virtiofs    - Force virtiofs.
//
enum class DrvFsTransport
{
    Default,
    Invalid,
    Plan9,
    Virtio9p,
    Virtiofs,
};

//
// Parses a transport=<value> token out of a DrvFs option string. Any matching
// token is removed from Options regardless of position. The return value is
// the parsed transport, or DrvFsTransport::Default if no override was
// supplied. Invalid or empty values return DrvFsTransport::Invalid. If multiple
// transport= tokens are present, the last one wins (the others are dropped).
//
DrvFsTransport ExtractTransportOption(std::string& Options);

int MountDrvfs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode = nullptr);

int MountDrvfsEntry(int Argc, char* Argv[]);

int MountPlan9Share(const char* Source, const char* Target, const char* Options, bool Admin, DrvFsTransport Transport = DrvFsTransport::Default, int* ExitCode = nullptr);

int MountPlan9(
    const char* Source,
    const char* Target,
    const char* Options,
    std::optional<bool> Admin,
    const wsl::linux::WslDistributionConfig& Config,
    int* ExitCode,
    DrvFsTransport Transport = DrvFsTransport::Default);

int MountVirtioFs(
    const char* Source,
    const char* Target,
    const char* Options,
    std::optional<bool> Admin,
    const wsl::linux::WslDistributionConfig& Config,
    int* ExitCode = nullptr,
    bool AllowFallback = true);

int RemountVirtioFs(const char* Tag, const char* Target, const char* Options, bool Admin);

std::string QueryVirtiofsMountSource(const char* Tag);
