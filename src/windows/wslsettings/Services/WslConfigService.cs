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
    private readonly Dictionary<WslConfigEntry, PendingSettingState> _pendingSettings = new();

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
        WslConfigSettingManaged? wslConfigSetting;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            if (!defaultSetting && _pendingSettings.TryGetValue(wslConfigEntry, out var pendingSettingState))
            {
                wslConfigSetting = pendingSettingState.PendingValue.Clone();
            }
            else
            {
                wslConfigSetting = GetPersistedWslConfigSetting(wslConfigEntry, defaultSetting);
            }
        }

        return wslConfigSetting;
    }

    public uint SetWslConfigSetting(IWslConfigSetting wslConfigSetting)
    {
        var settingManaged = wslConfigSetting as WslConfigSettingManaged;
        if (settingManaged == null)
        {
            throw new ArgumentNullException("wslConfigSetting");
        }

        bool hasPendingChangesChanged = false;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            var currentPersisted = GetPersistedWslConfigSetting(settingManaged.ConfigEntry, false);

            var hadPendingBefore = _pendingSettings.Count > 0;
            if (currentPersisted.Equals(settingManaged))
            {
                _pendingSettings.Remove(settingManaged.ConfigEntry);
            }
            else
            {
                if (_pendingSettings.TryGetValue(settingManaged.ConfigEntry, out var pendingSettingState))
                {
                    pendingSettingState.PendingValue = settingManaged.Clone();
                }
                else
                {
                    _pendingSettings[settingManaged.ConfigEntry] = new PendingSettingState
                    {
                        CurrentValue = currentPersisted,
                        PendingValue = settingManaged.Clone(),
                    };
                }
            }

            hasPendingChangesChanged = hadPendingBefore != (_pendingSettings.Count > 0);
        }

        OnPendingChangesChanged(hasPendingChangesChanged);
        return 0;
    }

    public bool HasPendingChanges
    {
        get
        {
            lock (_wslCoreConfigInterfaceLockObj!)
            {
                return _pendingSettings.Count > 0;
            }
        }
    }

    public IReadOnlyList<WslConfigPendingChange> GetPendingChanges()
    {
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            var changes = new List<WslConfigPendingChange>(_pendingSettings.Count);
            foreach (var pendingSettingState in _pendingSettings.Values)
            {
                changes.Add(new WslConfigPendingChange
                {
                    ConfigEntry = pendingSettingState.PendingValue.ConfigEntry,
                    CurrentSetting = pendingSettingState.CurrentValue,
                    PendingSetting = pendingSettingState.PendingValue,
                });
            }

            return changes;
        }
    }

    public uint CommitPendingChanges()
    {
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = false;
            try
            {
                foreach (var entry in _pendingSettings)
                {
                    var result = WslCoreConfigInterface.SetWslConfigSetting(_wslConfig, entry.Value.PendingValue.ConfigSetting);
                    if (result != 0)
                    {
                        return result;
                    }
                }

                _pendingSettings.Clear();
            }
            finally
            {
                _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
            }
        }

        _onPendingChangesChangedHandler?.Invoke();
        _onWslConfigChangedHandler?.Invoke();
        return 0;
    }

    public void ClearPendingChanges()
    {
        bool hadPendingChanges;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            hadPendingChanges = _pendingSettings.Count > 0;
            _pendingSettings.Clear();
        }

        OnPendingChangesChanged(hadPendingChanges);
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

    private PendingChangesChangedEventHandler? _onPendingChangesChangedHandler = null;
    public event PendingChangesChangedEventHandler PendingChangesChanged
    {
        add
        {
            _onPendingChangesChangedHandler += value;
        }
        remove
        {
            _onPendingChangesChangedHandler -= value;
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

    private WslConfigSettingManaged GetPersistedWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting)
    {
        return new WslConfigSettingManaged(WslCoreConfigInterface.GetWslConfigSetting(defaultSetting ? _wslConfigDefaults : _wslConfig, wslConfigEntry));
    }

    private void OnPendingChangesChanged(bool raiseEvent)
    {
        if (raiseEvent)
        {
            _onPendingChangesChangedHandler?.Invoke();
        }
    }

    private sealed class PendingSettingState
    {
        public required WslConfigSettingManaged CurrentValue { get; init; }
        public required WslConfigSettingManaged PendingValue { get; set; }
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

    public WslConfigSettingManaged Clone()
    {
        var nativeInternal = new WslConfigSetting.__Internal();
        nativeInternal.ConfigEntry = ConfigSetting.ConfigEntry;
        switch (ConfigSetting.ConfigEntry)
        {
        case WslConfigEntry.SwapFilePath:
        case WslConfigEntry.IgnoredPorts:
        case WslConfigEntry.KernelPath:
        case WslConfigEntry.SystemDistroPath:
        case WslConfigEntry.KernelModulesPath:
            // Create via the managed clone constructor so string memory is owned
            var clone = WslConfigSetting.__CreateInstance(nativeInternal);
            clone.StringValue = ConfigSetting.StringValue;
            return new WslConfigSettingManaged(clone);
        case WslConfigEntry.ProcessorCount:
        case WslConfigEntry.InitialAutoProxyTimeout:
        case WslConfigEntry.VMIdleTimeout:
            nativeInternal.Int32Value = ConfigSetting.Int32Value;
            break;
        case WslConfigEntry.MemorySizeBytes:
        case WslConfigEntry.SwapSizeBytes:
        case WslConfigEntry.VhdSizeBytes:
            nativeInternal.UInt64Value = ConfigSetting.UInt64Value;
            break;
        case WslConfigEntry.NetworkingMode:
            nativeInternal.NetworkingConfigurationValue = ConfigSetting.NetworkingConfigurationValue;
            break;
        case WslConfigEntry.AutoMemoryReclaim:
            nativeInternal.MemoryReclaimModeValue = ConfigSetting.MemoryReclaimModeValue;
            break;
        default:
            nativeInternal.BoolValue = (byte)(ConfigSetting.BoolValue ? 1 : 0);
            break;
        }

        return new WslConfigSettingManaged(WslConfigSetting.__CreateInstance(nativeInternal));
    }

    private object GetValueObject()
    {
        switch (ConfigSetting.ConfigEntry)
        {
        case WslConfigEntry.SwapFilePath:
        case WslConfigEntry.IgnoredPorts:
        case WslConfigEntry.KernelPath:
        case WslConfigEntry.SystemDistroPath:
        case WslConfigEntry.KernelModulesPath:
            return ConfigSetting.StringValue;
        case WslConfigEntry.ProcessorCount:
        case WslConfigEntry.InitialAutoProxyTimeout:
        case WslConfigEntry.VMIdleTimeout:
            return ConfigSetting.Int32Value;
        case WslConfigEntry.MemorySizeBytes:
        case WslConfigEntry.SwapSizeBytes:
        case WslConfigEntry.VhdSizeBytes:
            return ConfigSetting.UInt64Value;
        case WslConfigEntry.NetworkingMode:
            return ConfigSetting.NetworkingConfigurationValue;
        case WslConfigEntry.AutoMemoryReclaim:
            return ConfigSetting.MemoryReclaimModeValue;
        default:
            return ConfigSetting.BoolValue;
        }
    }

    public bool Equals(WslConfigSettingManaged other)
    {
        return Equals(other.GetValueObject());
    }

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