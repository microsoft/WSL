/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslDistributionConfig.h

Abstract:

    This file contains the WslDistributionConfig class definition.

--*/

#pragma once
#include <optional>
#include "p9tracelogging.h"
#include "lxinitshared.h"
#include "common.h"
#include "SocketChannel.h"

namespace wsl::linux {

constexpr auto c_ConfigAutoMountOption = "automount.enabled";
constexpr auto c_ConfigAutoUpdateTimezoneOption = "time.useWindowsTimezone";
constexpr auto c_ConfigBootCommandOption = "boot.command";
constexpr auto c_ConfigBootProtectBinfmtOption = "boot.protectBinfmt";
constexpr auto c_ConfigBootSystemdOption = "boot.systemd";
constexpr auto c_ConfigInteropAppendWindowsPathOption = "interop.appendWindowsPath";
constexpr auto c_ConfigInteropEnabledOption = "interop.enabled";
constexpr auto c_ConfigMountFsTabOption = "automount.mountFsTab";
constexpr auto c_ConfigGenerateHostsOption = "network.generateHosts";
constexpr auto c_ConfigGenerateResolvConfOption = "network.generateResolvConf";
constexpr auto c_ConfigEnableGuiAppsOption = "general.guiApplications";
constexpr auto c_ConfigPlan9EnabledOption = "fileServer.enabled";
constexpr auto c_ConfigAppendGpuLibPathOption = "gpu.appendLibPath";
constexpr auto c_ConfigGpuEnabledOption = "gpu.enabled";
constexpr auto c_ConfigLinkOsLibsOption = "automount.ldconfig";
constexpr auto c_ConfigAutoMountRoot = "automount.root";

struct WslDistributionConfig
{
    WslDistributionConfig(const char* configFilePath);

    WslDistributionConfig(const WslDistributionConfig&) = delete;
    WslDistributionConfig& operator=(const WslDistributionConfig&) = delete;

    WslDistributionConfig(WslDistributionConfig&&) = default;
    WslDistributionConfig& operator=(WslDistributionConfig&&) = default;

    enum class CGroupVersion
    {
        v1 = 0,
        v2 = 1
    };

    bool AutoMount = true;
    bool AutoUpdateTimezone = true;
    std::optional<std::string> BootCommand;
    bool BootInit = false;
    int BootInitTimeout = 10 * 1000;
    bool BootProtectBinfmt = true;
    std::optional<std::string> DefaultUser;
    std::string DrvFsPrefix = "/mnt";
    std::optional<std::string> DrvFsOptions;
    bool InteropAppendWindowsPath = true;
    bool InteropEnabled = true;
    bool MountFsTab = true;
    bool GenerateHosts = true;
    bool GenerateResolvConf = true;
    std::optional<std::string> HostName;
    bool Plan9Enabled = true;
    std::optional<std::string> Plan9LogFile;
    int Plan9LogLevel = TRACE_LEVEL_INFORMATION;
    bool Plan9LogTruncate = true;
    int Umask = 0022;
    bool AppendGpuLibPath = true;
    bool GpuEnabled = true;
    bool LinkOsLibs = true;
    CGroupVersion CGroup = CGroupVersion::v2;

    //
    // Values not set by /etc/wsl.conf.
    //

    bool GuiAppsEnabled = false;
    std::optional<int> FeatureFlags;
    std::optional<LX_MINI_INIT_NETWORKING_MODE> NetworkingMode;
    std::optional<std::string> VmId;

    //
    // Global state for boot state. The socket is used to delay-start the distro init process
    // to when the first session leader is created.
    //

    wil::unique_fd BootStartWriteSocket;
    wsl::shared::SocketChannel Plan9ControlChannel;
    std::optional<pid_t> InitPid;
};

} // namespace wsl::linux