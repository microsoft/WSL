// Copyright (C) Microsoft Corporation. All rights reserved.

using System.Linq;
using WslSettings.Contracts.Services;
using static WslSettings.Contracts.Services.IWslConfigService;

namespace WslSettings.Services;

public class WslConfigService : IWslConfigService
{
    private WslConfig? _wslConfig { get; set; }
    private WslConfig? _wslConfigDefaults { get; init; }
    private readonly object? _wslCoreConfigInterfaceLockObj = null;
    private FileSystemWatcher? _wslConfigFileSystemWatcher = null;

    // We keep two copies of every config value: _baselineSnapshot has what's on disk,
    // _workingSnapshot has the user's in-app edits. Comparing the two tells us what changed.
    private readonly Dictionary<WslConfigEntry, WslConfigSettingManaged> _baselineSnapshot = new();
    private readonly Dictionary<WslConfigEntry, WslConfigSettingManaged> _workingSnapshot = new();

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

        // Read all current config values into both snapshots so they start in sync
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            RefreshSnapshots_NoLock();
        }
    }

    ~WslConfigService()
    {
        DisposeSnapshot(_baselineSnapshot);
        DisposeSnapshot(_workingSnapshot);
        WslCoreConfigInterface.FreeWslConfig(_wslConfig);
        WslCoreConfigInterface.FreeWslConfig(_wslConfigDefaults);
    }

    public IWslConfigSetting GetWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting)
    {
        // Default settings (for reset buttons) come straight from the native defaults config
        if (defaultSetting)
        {
            return GetPersistedWslConfigSetting(wslConfigEntry, defaultSetting: true);
        }

        lock (_wslCoreConfigInterfaceLockObj!)
        {
            var workingSettingValue = GetConfigSettingValueFromSnapshot(_workingSnapshot, wslConfigEntry, nameof(_workingSnapshot));
            return workingSettingValue.Clone();
        }
    }

    public uint SetWslConfigSetting(IWslConfigSetting wslConfigSetting)
    {
        var settingManaged = wslConfigSetting as WslConfigSettingManaged;
        if (settingManaged == null)
        {
            throw new ArgumentNullException(nameof(wslConfigSetting));
        }

        bool pendingChanged;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            // Track whether pending state toggles so we can show/hide the Apply button
            var hadPendingBefore = HasPendingChanges_NoLock();

            // Replace the old value in the working snapshot with the new one.
            // We clone to avoid sharing the same native object with the caller.
            if (_workingSnapshot.TryGetValue(settingManaged.ConfigEntry, out var existingSettingValue))
            {
                existingSettingValue.ConfigSetting.Dispose();
            }
            _workingSnapshot[settingManaged.ConfigEntry] = settingManaged.Clone();

            pendingChanged = hadPendingBefore != HasPendingChanges_NoLock();
        }

        OnPendingChangesChanged(pendingChanged);
        return 0;
    }

    public bool HasPendingChanges
    {
        get
        {
            lock (_wslCoreConfigInterfaceLockObj!)
            {
                return HasPendingChanges_NoLock();
            }
        }
    }

    public IReadOnlyList<WslConfigPendingChange> GetPendingChanges()
    {
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            return EnumeratePendingChanges_NoLock().ToList();
        }
    }

    public uint CommitPendingChanges()
    {
        uint result = 0;
        bool pendingStateChanged = false;

        lock (_wslCoreConfigInterfaceLockObj!)
        {
            var pendingChanges = EnumeratePendingChanges_NoLock().ToList();
            if (pendingChanges.Count == 0)
            {
                return 0;
            }

            var hadPendingBefore = HasPendingChanges_NoLock();
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = false;
            try
            {
                foreach (var change in pendingChanges)
                {
                    result = WslCoreConfigInterface.SetWslConfigSetting(_wslConfig, ((WslConfigSettingManaged)change.PendingSetting).ConfigSetting);
                    if (result != 0)
                    {
                        break;
                    }
                }

                ReloadConfig_NoLock();
                if (result == 0)
                {
                    // Full success - reset both snapshots from disk
                    RefreshSnapshots_NoLock();
                }
                else
                {
                    // Partial failure - only refresh baseline so successfully-applied
                    // entries drop out of pending, but unapplied entries remain.
                    RefreshBaselineSnapshot_NoLock();
                }

                var hasPendingAfter = HasPendingChanges_NoLock();
                // This controls whether the Apply button appears or disappears in the UI
                pendingStateChanged = hadPendingBefore != hasPendingAfter;
            }
            finally
            {
                foreach (var change in pendingChanges)
                {
                    ((WslConfigSettingManaged)change.CurrentSetting).ConfigSetting.Dispose();
                    ((WslConfigSettingManaged)change.PendingSetting).ConfigSetting.Dispose();
                }

                _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
            }
        }

        if (pendingStateChanged)
        {
            _onPendingChangesChangedHandler?.Invoke();
        }

        // Always raise config-changed when we write to disk (even partial)
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
        bool pendingStateChanged;
        lock (_wslCoreConfigInterfaceLockObj!)
        {
            // Someone edited .wslconfig outside the app — reload and reset both snapshots.
            // Any in-app pending changes are dropped.
            var hadPendingBefore = HasPendingChanges_NoLock();
            _wslConfigFileSystemWatcher!.EnableRaisingEvents = false;
            try
            {
                ReloadConfig_NoLock();
                RefreshSnapshots_NoLock();
            }
            finally
            {
                _wslConfigFileSystemWatcher!.EnableRaisingEvents = true;
            }

            var hasPendingAfter = HasPendingChanges_NoLock();
            pendingStateChanged = hadPendingBefore != hasPendingAfter;
        }

        if (pendingStateChanged)
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

    // Fire the PendingChangesChanged event only when the pending state actually toggled.
    private void OnPendingChangesChanged(bool raiseEvent)
    {
        if (raiseEvent)
        {
            _onPendingChangesChangedHandler?.Invoke();
        }
    }

    // Every WslConfigEntry value (excluding the sentinel NoEntry), cached once so we don't re-enumerate the enum each time.
    private static readonly IReadOnlyList<WslConfigEntry> AllConfigEntries =
        Enum.GetValues(typeof(WslConfigEntry))
            .Cast<WslConfigEntry>()
            .Where(entry => entry != WslConfigEntry.NoEntry)
            .ToList();

    // Free the native memory behind each setting in a snapshot and clear the dictionary.
    private static void DisposeSnapshot(IDictionary<WslConfigEntry, WslConfigSettingManaged> snapshot)
    {
        foreach (var setting in snapshot.Values)
        {
            setting?.ConfigSetting?.Dispose();
        }
        snapshot.Clear();
    }

    // Look up an entry in a snapshot. Both snapshots are populated with every entry at startup,
    // so a missing key means something went wrong during initialization.
    private static WslConfigSettingManaged GetConfigSettingValueFromSnapshot(IDictionary<WslConfigEntry, WslConfigSettingManaged> snapshot, WslConfigEntry entry, string snapshotName)
    {
        if (!snapshot.TryGetValue(entry, out var setting) || setting is null)
        {
            throw new InvalidOperationException($"Snapshot '{snapshotName}' missing entry '{entry}'. This should never happen — snapshots are populated for every entry at startup.");
        }
        return setting;
    }

    // Re-read every config value from disk into both snapshots.
    // After this, baseline == working, so there are no pending changes.
    private void RefreshSnapshots_NoLock()
    {
        DisposeSnapshot(_baselineSnapshot);
        DisposeSnapshot(_workingSnapshot);
        foreach (var entry in AllConfigEntries)
        {
            var persisted = GetPersistedWslConfigSetting(entry, defaultSetting: false);
            _baselineSnapshot[entry] = persisted;
            _workingSnapshot[entry] = persisted.Clone();
        }
    }

    // Re-read only the baseline snapshot from disk, leaving the working snapshot intact.
    // Used on partial commit failure so successfully-applied entries drop out of pending
    // while unapplied entries remain.
    private void RefreshBaselineSnapshot_NoLock()
    {
        DisposeSnapshot(_baselineSnapshot);
        foreach (var entry in AllConfigEntries)
        {
            _baselineSnapshot[entry] = GetPersistedWslConfigSetting(entry, defaultSetting: false);
        }
    }

    // Walk every entry and check if the user's in-app value differs from what's on disk.
    private bool HasPendingChanges_NoLock()
    {
        foreach (var entry in AllConfigEntries)
        {
            var baseline = GetConfigSettingValueFromSnapshot(_baselineSnapshot, entry, nameof(_baselineSnapshot));
            var working = GetConfigSettingValueFromSnapshot(_workingSnapshot, entry, nameof(_workingSnapshot));

            if (!baseline.Equals(working))
            {
                return true;
            }
        }

        return false;
    }

    // Same as HasPendingChanges_NoLock but returns the actual before/after values for each changed entry.
    // Used to build the list shown in the Apply dialog.
    private IEnumerable<WslConfigPendingChange> EnumeratePendingChanges_NoLock()
    {
        foreach (var entry in AllConfigEntries)
        {
            var baseline = GetConfigSettingValueFromSnapshot(_baselineSnapshot, entry, nameof(_baselineSnapshot));
            var working = GetConfigSettingValueFromSnapshot(_workingSnapshot, entry, nameof(_workingSnapshot));

            if (!baseline.Equals(working))
            {
                    yield return new WslConfigPendingChange
                    {
                        ConfigEntry = entry,
                        CurrentSetting = baseline.Clone(),
                        PendingSetting = working.Clone(),
                    };
            }
        }
    }

    // Re-read .wslconfig from disk. Called before RefreshSnapshots_NoLock to pick up external edits.
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
            var stringValue = ConfigSetting.StringValue;
            if (stringValue is not null)
            {
                clone.StringValue = stringValue;
            }
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

    public bool Equals(WslConfigSettingManaged other)
    {
        if (other is null)
        {
            return false;
        }

        if (ConfigSetting.ConfigEntry != other.ConfigSetting.ConfigEntry)
        {
            return false;
        }

        switch (ConfigSetting.ConfigEntry)
        {
        case WslConfigEntry.SwapFilePath:
        case WslConfigEntry.IgnoredPorts:
        case WslConfigEntry.KernelPath:
        case WslConfigEntry.SystemDistroPath:
        case WslConfigEntry.KernelModulesPath:
            return string.Equals(
                ConfigSetting.StringValue,
                other.ConfigSetting.StringValue,
                System.StringComparison.Ordinal);
        case WslConfigEntry.ProcessorCount:
        case WslConfigEntry.InitialAutoProxyTimeout:
        case WslConfigEntry.VMIdleTimeout:
            return ConfigSetting.Int32Value == other.ConfigSetting.Int32Value;
        case WslConfigEntry.MemorySizeBytes:
        case WslConfigEntry.SwapSizeBytes:
        case WslConfigEntry.VhdSizeBytes:
            return ConfigSetting.UInt64Value == other.ConfigSetting.UInt64Value;
        case WslConfigEntry.NetworkingMode:
            return ConfigSetting.NetworkingConfigurationValue == other.ConfigSetting.NetworkingConfigurationValue;
        case WslConfigEntry.AutoMemoryReclaim:
            return ConfigSetting.MemoryReclaimModeValue == other.ConfigSetting.MemoryReclaimModeValue;
        default:
            return ConfigSetting.BoolValue == other.ConfigSetting.BoolValue;
        }
    }

#nullable enable
    public uint SetValue(object? value)
    {
        if (value == null)
        {
            throw new ArgumentNullException(nameof(value));
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
            throw new ArgumentNullException(nameof(value));
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