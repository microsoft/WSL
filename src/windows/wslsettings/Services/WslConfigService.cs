// Copyright (C) Microsoft Corporation. All rights reserved.

using WslSettings.Contracts.Services;
using static WslSettings.Contracts.Services.IWslConfigService;

namespace WslSettings.Services;

public class WslConfigService : IWslConfigService
{
    private WslConfig? _wslConfig { get; set; }
    private WslConfig? _wslConfigDefaults { get; init; }
    private readonly object? _wslCoreConfigInterfaceLockObj = null;
    private FileSystemWatcher? _wslConfigFileSystemWatcher = null;

    public WslConfigService()
    {
        string filePath = WslCoreConfigInterface.GetWslConfigFilePath();
        _wslConfig = WslCoreConfigInterface.CreateWslConfig(filePath);
        _wslConfigDefaults = WslCoreConfigInterface.CreateWslConfig(null);
        _wslCoreConfigInterfaceLockObj = new object();
        _wslConfigFileSystemWatcher = new FileSystemWatcher(Path.GetDirectoryName(filePath) ?? string.Empty, Path.GetFileName(filePath));

        _wslConfigFileSystemWatcher.NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite;

        _wslConfigFileSystemWatcher.Changed += OnWslConfigFileChanged;
        _wslConfigFileSystemWatcher.Deleted += OnWslConfigFileChanged;
        _wslConfigFileSystemWatcher.Renamed += OnWslConfigFileChanged;

        _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
    }

    ~WslConfigService()
    {
        WslCoreConfigInterface.FreeWslConfig(_wslConfig);
        WslCoreConfigInterface.FreeWslConfig(_wslConfigDefaults);
    }

    public IWslConfigSetting GetWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting)
    {
        WslConfigSettingManaged? wslConfigSetting = null;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            wslConfigSetting = new WslConfigSettingManaged(WslCoreConfigInterface.GetWslConfigSetting(defaultSetting ? _wslConfigDefaults : _wslConfig, wslConfigEntry));
        }

        return wslConfigSetting;
    }

    public uint SetWslConfigSetting(IWslConfigSetting wslConfigSetting)
    {
        var wslConfigSettingsManaged = wslConfigSetting as WslConfigSettingManaged;
        if (wslConfigSettingsManaged == null)
        {
            throw new ArgumentNullException("wslConfigSetting");
        }

        uint result = 0;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = false;
            result = WslCoreConfigInterface.SetWslConfigSetting(_wslConfig, wslConfigSettingsManaged.ConfigSetting);
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
        }

        return result;
    }

    private WslConfigChangedEventHandler? _onWslConfigChangedHandler = null;
    public event WslConfigChangedEventHandler WslConfigChanged
    {
        add
        {
            _onWslConfigChangedHandler += value;
        }
        remove
        {
            _onWslConfigChangedHandler -= value;
        }
    }

    private void OnWslConfigFileChanged(object sender, FileSystemEventArgs e)
    {
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = false;
            WslCoreConfigInterface.FreeWslConfig(_wslConfig);
            _wslConfig = WslCoreConfigInterface.CreateWslConfig(WslCoreConfigInterface.GetWslConfigFilePath());
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
        }

        _onWslConfigChangedHandler?.Invoke();
    }
}

public partial class WslConfigSettingManaged : IWslConfigSetting
{
    public WslConfigSettingManaged(WslConfigSetting wslConfigSetting)
    {
        ConfigSetting = wslConfigSetting;
    }

    ~WslConfigSettingManaged()
    {
        ConfigSetting.Dispose();
    }

    public WslConfigSetting ConfigSetting { get; init; }
    public WslConfigEntry ConfigEntry { get { return ConfigSetting.ConfigEntry; } }
    public string StringValue { get { return ConfigSetting.StringValue; } }
    public ulong UInt64Value { get { return ConfigSetting.UInt64Value; } }
    public int Int32Value { get { return ConfigSetting.Int32Value; } }
    public bool BoolValue { get { return ConfigSetting.BoolValue; } }
    public NetworkingConfiguration NetworkingConfigurationValue { get { return ConfigSetting.NetworkingConfigurationValue; } }
    public MemoryReclaimMode MemoryReclaimModeValue { get { return ConfigSetting.MemoryReclaimModeValue; } }

#nullable enable
    public uint SetValue(object? value)
    {
        if (value == null)
        {
            throw new ArgumentNullException("value");
        }

        if ("".GetType() == value.GetType())
        {
            ConfigSetting.StringValue = (string)value;
        }
        else if (ConfigSetting.UInt64Value.GetType() == value.GetType())
        {
            ConfigSetting.UInt64Value = (ulong)value;
        }
        else if (ConfigSetting.Int32Value.GetType() == value.GetType())
        {
            ConfigSetting.Int32Value = (int)value;
        }
        else if (ConfigSetting.BoolValue.GetType() == value.GetType())
        {
            ConfigSetting.BoolValue = (bool)value;
        }
        else if (ConfigSetting.NetworkingConfigurationValue.GetType() == value.GetType())
        {
            ConfigSetting.NetworkingConfigurationValue = (NetworkingConfiguration)value;
        }
        else if (ConfigSetting.MemoryReclaimModeValue.GetType() == value.GetType())
        {
            ConfigSetting.MemoryReclaimModeValue = (MemoryReclaimMode)value;
        }
        else
        {
            throw new InvalidDataException();
        }

        return App.GetService<IWslConfigService>().SetWslConfigSetting(this);
    }

    public override bool Equals(object? value)
    {
        if (value == null)
        {
            throw new ArgumentNullException("value");
        }

        // Special handling for byte values. Compare using MB since in the UI the user works with MB.
        if (ConfigSetting.ConfigEntry == WslConfigEntry.MemorySizeBytes ||
            ConfigSetting.ConfigEntry == WslConfigEntry.SwapSizeBytes ||
            ConfigSetting.ConfigEntry == WslConfigEntry.VhdSizeBytes)
        {
            return ((ulong)value / Constants.MB) == (UInt64Value / Constants.MB);
        }

        if ("".GetType() == value.GetType())
        {
            return ConfigSetting.StringValue == (string)value;
        }
        else if (ConfigSetting.UInt64Value.GetType() == value.GetType())
        {
            return ConfigSetting.UInt64Value == (ulong)value;
        }
        else if (ConfigSetting.Int32Value.GetType() == value.GetType())
        {
            return ConfigSetting.Int32Value == (int)value;
        }
        else if (ConfigSetting.BoolValue.GetType() == value.GetType())
        {
            return ConfigSetting.BoolValue == (bool)value;
        }
        else if (ConfigSetting.NetworkingConfigurationValue.GetType() == value.GetType())
        {
            return ConfigSetting.NetworkingConfigurationValue == (NetworkingConfiguration)value;
        }
        else if (ConfigSetting.MemoryReclaimModeValue.GetType() == value.GetType())
        {
            return ConfigSetting.MemoryReclaimModeValue == (MemoryReclaimMode)value;
        }
        else
        {
            throw new InvalidDataException();
        }
    }

    public override int GetHashCode()
    {
        return base.GetHashCode();
    }
}