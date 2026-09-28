// Copyright (C) Microsoft Corporation. All rights reserved.

using WslSettings.Contracts.Services;
using static WslSettings.Contracts.Services.IWslConfigService;

namespace WslSettings.Services;

public class WslConfigService : IWslConfigService, IDisposable
{
    private WslConfig? _wslConfig { get; set; }
    private WslConfig? _wslConfigDefaults { get; init; }
    private readonly object? _wslCoreConfigInterfaceLockObj = null;
    private FileSystemWatcher? _wslConfigFileSystemWatcher = null;

    // Pending changes: stores only entries the user has changed in-app but not yet committed.
    // Values are plain managed objects (bool, int, ulong, string, or enum) — no native clones needed.
    private readonly Dictionary<WslConfigEntry, object> _pendingValues = new();

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
        Dispose(false);
    }

    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    protected virtual void Dispose(bool disposing)
    {
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            if (disposing && _wslConfigFileSystemWatcher != null)
            {
                _wslConfigFileSystemWatcher.EnableRaisingEvents = false;
                _wslConfigFileSystemWatcher.Changed -= OnWslConfigFileChanged;
                _wslConfigFileSystemWatcher.Deleted -= OnWslConfigFileChanged;
                _wslConfigFileSystemWatcher.Renamed -= OnWslConfigFileChanged;
                _wslConfigFileSystemWatcher.Dispose();
                _wslConfigFileSystemWatcher = null;
            }

            WslCoreConfigInterface.FreeWslConfig(_wslConfig);
            WslCoreConfigInterface.FreeWslConfig(_wslConfigDefaults);
        }
    }

    public IWslConfigSetting GetWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting)
    {
        if (defaultSetting)
        {
            return GetPersistedWslConfigSetting(wslConfigEntry, defaultSetting: true);
        }

        lock (_wslCoreConfigInterfaceLockObj!)
        {
            if (_pendingValues.TryGetValue(wslConfigEntry, out var pendingValue))
            {
                // Build a native setting from the pending managed value for the caller
                var setting = GetPersistedWslConfigSetting(wslConfigEntry, defaultSetting: false);
                setting.SetValueDirect(pendingValue);
                return setting;
            }

            return GetPersistedWslConfigSetting(wslConfigEntry, defaultSetting: false);
        }
    }

    public uint SetWslConfigSetting(IWslConfigSetting wslConfigSetting)
    {
        var settingManaged = wslConfigSetting as WslConfigSettingManaged;
        if (settingManaged == null)
        {
            throw new ArgumentNullException(nameof(wslConfigSetting));
        }

        bool pendingStateChanged;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            var hadPendingBefore = _pendingValues.Count > 0;

            var persisted = GetPersistedWslConfigSetting(settingManaged.ConfigEntry, defaultSetting: false);
            try
            {
                bool isChanged = !persisted.Equals(settingManaged.GetValueAsObject());

                if (isChanged)
                {
                    _pendingValues[settingManaged.ConfigEntry] = settingManaged.GetValueAsObject();
                }
                else
                {
                    _pendingValues.Remove(settingManaged.ConfigEntry);
                }
            }
            finally
            {
                persisted.ConfigSetting.Dispose();
            }

            var hasPendingAfter = _pendingValues.Count > 0;
            pendingStateChanged = hadPendingBefore != hasPendingAfter;
        }

        if (pendingStateChanged)
        {
            _onPendingChangesChangedHandler?.Invoke();
        }

        return 0;
    }

    public bool HasPendingChanges
    {
        get
        {
            lock (_wslCoreConfigInterfaceLockObj!)
            {
                return _pendingValues.Count > 0;
            }
        }
    }

    public IReadOnlyList<WslConfigPendingChange> GetPendingChanges()
    {
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            var changes = new List<WslConfigPendingChange>(_pendingValues.Count);
            foreach (var (entry, value) in _pendingValues)
            {
                changes.Add(new WslConfigPendingChange
                {
                    ConfigEntry = entry,
                    PendingValue = value,
                });
            }
            return changes;
        }
    }

    public uint CommitPendingChanges()
    {
        uint result = 0;
        bool pendingStateChanged;

        lock (_wslCoreConfigInterfaceLockObj!)
        {
            if (_pendingValues.Count == 0)
            {
                return 0;
            }

            _wslConfigFileSystemWatcher!.EnableRaisingEvents = false;
            try
            {
                var committed = new List<WslConfigEntry>();
                foreach (var (entry, value) in _pendingValues)
                {
                    var setting = GetPersistedWslConfigSetting(entry, defaultSetting: false);
                    try
                    {
                        setting.SetValueDirect(value);
                        result = WslCoreConfigInterface.SetWslConfigSetting(_wslConfig, setting.ConfigSetting);
                    }
                    finally
                    {
                        setting.ConfigSetting.Dispose();
                    }

                    if (result != 0)
                    {
                        break;
                    }

                    committed.Add(entry);
                }

                ReloadConfig_NoLock();

                if (result == 0)
                {
                    _pendingValues.Clear();
                }
                else
                {
                    // Partial failure - only remove successfully-committed entries
                    // so unapplied entries remain pending.
                    foreach (var entry in committed)
                    {
                        _pendingValues.Remove(entry);
                    }
                }
            }
            finally
            {
                _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
            }

            // We entered with pending changes. Fire only if they're now empty (full success).
            pendingStateChanged = _pendingValues.Count == 0;
        }

        if (pendingStateChanged)
        {
            _onPendingChangesChangedHandler?.Invoke();
        }
        _onWslConfigChangedHandler?.Invoke();
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
        bool hadPending;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            hadPending = _pendingValues.Count > 0;
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = false;
            try
            {
                ReloadConfig_NoLock();
                _pendingValues.Clear();
            }
            finally
            {
                _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
            }
        }

        if (hadPending)
        {
            _onPendingChangesChangedHandler?.Invoke();
        }
        _onWslConfigChangedHandler?.Invoke();
    }

    // Read a single setting from the native config layer (either the user's .wslconfig or built-in defaults).
    private WslConfigSettingManaged GetPersistedWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting)
    {
        return new WslConfigSettingManaged(WslCoreConfigInterface.GetWslConfigSetting(defaultSetting ? _wslConfigDefaults : _wslConfig, wslConfigEntry));
    }

    private void ReloadConfig_NoLock()
    {
        WslCoreConfigInterface.FreeWslConfig(_wslConfig);
        _wslConfig = WslCoreConfigInterface.CreateWslConfig(WslCoreConfigInterface.GetWslConfigFilePath());
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

    public object GetValueAsObject()
    {
        switch (ConfigSetting.ConfigEntry.GetValueKind())
        {
            case WslConfigValueKind.String:
                return ConfigSetting.StringValue;
            case WslConfigValueKind.Int32:
                return ConfigSetting.Int32Value;
            case WslConfigValueKind.UInt64:
                return ConfigSetting.UInt64Value;
            case WslConfigValueKind.NetworkingConfiguration:
                return ConfigSetting.NetworkingConfigurationValue;
            case WslConfigValueKind.MemoryReclaimMode:
                return ConfigSetting.MemoryReclaimModeValue;
            default:
                return ConfigSetting.BoolValue;
        }
    }

    // Apply a plain managed value to the native setting without going through the service.
    public void SetValueDirect(object value)
    {
        switch (ConfigSetting.ConfigEntry.GetValueKind())
        {
            case WslConfigValueKind.String:
                ConfigSetting.StringValue = (string)value;
                break;
            case WslConfigValueKind.Int32:
                ConfigSetting.Int32Value = (int)value;
                break;
            case WslConfigValueKind.UInt64:
                ConfigSetting.UInt64Value = (ulong)value;
                break;
            case WslConfigValueKind.NetworkingConfiguration:
                ConfigSetting.NetworkingConfigurationValue = (NetworkingConfiguration)value;
                break;
            case WslConfigValueKind.MemoryReclaimMode:
                ConfigSetting.MemoryReclaimModeValue = (MemoryReclaimMode)value;
                break;
            default:
                ConfigSetting.BoolValue = (bool)value;
                break;
        }
    }

#nullable enable
    public uint SetValue(object? value)
    {
        if (value == null)
        {
            throw new ArgumentNullException(nameof(value));
        }

        switch (ConfigSetting.ConfigEntry.GetValueKind())
        {
            case WslConfigValueKind.String:
                ConfigSetting.StringValue = (string)value;
                break;
            case WslConfigValueKind.Int32:
                ConfigSetting.Int32Value = (int)value;
                break;
            case WslConfigValueKind.UInt64:
                ConfigSetting.UInt64Value = (ulong)value;
                break;
            case WslConfigValueKind.NetworkingConfiguration:
                ConfigSetting.NetworkingConfigurationValue = (NetworkingConfiguration)value;
                break;
            case WslConfigValueKind.MemoryReclaimMode:
                ConfigSetting.MemoryReclaimModeValue = (MemoryReclaimMode)value;
                break;
            default:
                ConfigSetting.BoolValue = (bool)value;
                break;
        }

        return App.GetService<IWslConfigService>().SetWslConfigSetting(this);
    }

    public override bool Equals(object? value)
    {
        if (value == null)
        {
            throw new ArgumentNullException(nameof(value));
        }

        // Special handling for byte values. Compare using MB since in the UI the user works with MB.
        if (ConfigSetting.ConfigEntry == WslConfigEntry.MemorySizeBytes ||
            ConfigSetting.ConfigEntry == WslConfigEntry.SwapSizeBytes ||
            ConfigSetting.ConfigEntry == WslConfigEntry.VhdSizeBytes)
        {
            return ((ulong)value / Constants.MB) == (UInt64Value / Constants.MB);
        }

        // object.Equals handles null on either side (returns true if both null, false if one null).
        return object.Equals(GetValueAsObject(), value);
    }

    public override int GetHashCode()
    {
        return base.GetHashCode();
    }
}
