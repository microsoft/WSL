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
// Directory under which aggregate virtio-fs devices are mounted by their
// tag (one subdirectory per tag). Aggregate child bind-mounts source from
// VIRTIOFS_AGGREGATE_ROOT_DIR/<Tag>/<Subname>. Exposed here so that other
// init components can filter these internal mounts out of drvfs enumeration.
//
#define VIRTIOFS_AGGREGATE_ROOT_DIR "/run/wsl/virtiofs-root"

int MountDrvfs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode = nullptr);

int MountDrvfsEntry(int Argc, char* Argv[]);

int MountPlan9Share(const char* Source, const char* Target, const char* Options, bool Admin, int* ExitCode = nullptr);

int MountPlan9(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode);

int MountVirtioFs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode = nullptr);

int RemountVirtioFs(const char* Tag, const char* Subname, const char* Target, const char* Options, bool Admin);

//
// Look up the Windows path mounted by a virtio-fs share.
//
// Tag identifies the (aggregate) virtio-fs device tag and Subname
// identifies the child entry inside the device's synthetic root.
// Subname must be empty for legacy direct-mount shares.
//
std::string QueryVirtiofsMountSource(const char* Tag, const char* Subname = "");

//
// Bind-mount one child of an aggregate virtio-fs device onto Target.
//
// Tag is the aggregate device tag (GUID formatted) and Subname is the
// child entry inside the device's synthetic root (32 lowercase hex
// chars). Ensures the device is mounted at VIRTIOFS_AGGREGATE_ROOT_DIR/
// <Tag>, then bind-mounts <Tag>/<Subname> onto Target and applies
// MountOptions. Used by both drvfs and the WSLc Windows-folder path.
//
int MountVirtioFsAggregateChild(const char* Tag, const char* Subname, const char* Target, const char* MountOptions);
