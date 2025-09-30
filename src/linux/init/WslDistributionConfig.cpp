/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslDistributionConfig.h

Abstract:

    This file contains the WslDistributionConfig class implementation.

--*/

#include "WslDistributionConfig.h"
#include "configfile.h"
#include "util.h"

using wsl::linux::WslDistributionConfig;
using namespace wsl::linux;

WslDistributionConfig::WslDistributionConfig(const char* configFilePath)
{

    std::vector<ConfigKey> keys = {
        ConfigKey(c_ConfigAutoMountOption, AutoMount),
        ConfigKey(c_ConfigAutoMountRoot, DrvFsPrefix),
        ConfigKey("automount.options", DrvFsOptions),
        ConfigKey(c_ConfigMountFsTabOption, MountFsTab),
        ConfigKey(c_ConfigLinkOsLibsOption, LinkOsLibs),
        ConfigKey("automount.cgroups", {{"v1", CGroupVersion::v1}, {"v2", CGroupVersion::v2}}, CGroup, nullptr),

        ConfigKey("filesystem.umask", Umask),

        ConfigKey(c_ConfigInteropAppendWindowsPathOption, InteropAppendWindowsPath),
        ConfigKey(c_ConfigInteropEnabledOption, InteropEnabled),

        ConfigKey(c_ConfigGenerateHostsOption, GenerateHosts),
        ConfigKey(c_ConfigGenerateResolvConfOption, GenerateResolvConf),
        ConfigKey("network.hostname", HostName),

        ConfigKey(c_ConfigAutoUpdateTimezoneOption, AutoUpdateTimezone),

        ConfigKey(c_ConfigPlan9EnabledOption, Plan9Enabled),
        ConfigKey("fileServer.logFile", Plan9LogFile),
        ConfigKey("fileServer.logLevel", Plan9LogLevel),
        ConfigKey("fileServer.logTruncate", Plan9LogTruncate),

        ConfigKey(c_ConfigGpuEnabledOption, GpuEnabled),
        ConfigKey(c_ConfigAppendGpuLibPathOption, AppendGpuLibPath),

        ConfigKey("user.default", DefaultUser),

        ConfigKey(c_ConfigBootCommandOption, BootCommand),
        ConfigKey(c_ConfigBootSystemdOption, BootInit),
        ConfigKey("boot.initTimeout", BootInitTimeout),
        ConfigKey(c_ConfigBootProtectBinfmtOption, BootProtectBinfmt),

        ConfigKey(c_ConfigEnableGuiAppsOption, GuiAppsEnabled),
    };

    //
    // If the config file does not exist, then ParseConfigFile sets all the configuration
    // values to their defaults.
    //

    wil::unique_file File{fopen(configFilePath, "r")};
    ParseConfigFile(keys, File.get(), CFG_SKIP_UNKNOWN_VALUES, STRING_TO_WSTRING(CONFIG_FILE));

    //
    // Ensure the DrvFs prefix is well-formed (not empty and ends with a path separator).
    //

    std::string Prefix;
    if (!DrvFsPrefix.empty())
    {
        Prefix = DrvFsPrefix;
    }

    if (Prefix.empty() || Prefix.back() != '/')
    {
        Prefix += '/';
        DrvFsPrefix = Prefix;
    }

    //
    // Using boot.systemd is only supported on WSL2.
    //

    BootInit &= UtilIsUtilityVm();
    if (BootInit && (access(INIT_PATH, X_OK) < 0))
    {
        LOG_WARNING("access({}) failed {} - {} disabled", INIT_PATH, errno, c_ConfigBootSystemdOption);
        BootInit = false;
    }

    //
    // Apply safe mode overrides.
    //

    const char* Value = getenv(LX_WSL2_SAFE_MODE);
    if (Value && (strcmp(Value, "1") == 0))
    {
        auto DisableBoolOption = [&](const char* Option, bool& ConfigValue) {
            if (ConfigValue)
            {
                LOG_WARNING("{} - {} disabled", WSL_SAFE_MODE_WARNING, Option);
                ConfigValue = false;
            }
        };

        bool BootCommandEnabled = (BootCommand.has_value());
        for (const auto& ConfigOption : std::vector<std::pair<const char*, bool&>>{
                 {c_ConfigAutoMountOption, AutoMount},
                 {c_ConfigLinkOsLibsOption, LinkOsLibs},
                 {c_ConfigMountFsTabOption, MountFsTab},
                 {c_ConfigBootCommandOption, BootCommandEnabled},
                 {c_ConfigBootSystemdOption, BootInit},
                 {c_ConfigGenerateHostsOption, GenerateHosts},
                 {c_ConfigGenerateResolvConfOption, GenerateResolvConf},
                 {c_ConfigPlan9EnabledOption, Plan9Enabled},
                 {c_ConfigAppendGpuLibPathOption, AppendGpuLibPath},
                 {c_ConfigGpuEnabledOption, GpuEnabled},
                 {c_ConfigInteropAppendWindowsPathOption, InteropAppendWindowsPath},
                 {c_ConfigInteropEnabledOption, InteropEnabled},
                 {c_ConfigAutoUpdateTimezoneOption, AutoUpdateTimezone}})
        {
            DisableBoolOption(ConfigOption.first, ConfigOption.second);
        }

        BootCommand = {};
    }
}