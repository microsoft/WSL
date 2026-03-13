// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Contracts.Services;

public interface IWslConfigService
{
    IWslConfigSetting GetWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting = false);

    /// <summary>
    /// Sets a WSL config value in the in-memory working snapshot. Pending state is computed by diffing that working snapshot
    /// against a baseline captured at startup/last commit; values are only written to disk on <see cref="CommitPendingChanges"/>.
    /// </summary>
    uint SetWslConfigSetting(IWslConfigSetting setting);

    /// <summary>
    /// Indicates whether the working snapshot differs from the baseline snapshot (i.e., whether there are pending changes).
    /// </summary>
    bool HasPendingChanges { get; }

    /// <summary>
    /// Returns the diff (baseline vs working) for all entries that differ.
    /// Throws if snapshots are not fully hydrated (fail-fast rather than silently skipping).
    /// </summary>
    IReadOnlyList<WslConfigPendingChange> GetPendingChanges();
    uint CommitPendingChanges();
    public delegate void WslConfigChangedEventHandler();
    event WslConfigChangedEventHandler WslConfigChanged;
    public delegate void PendingChangesChangedEventHandler();
    event PendingChangesChangedEventHandler PendingChangesChanged;
}

public sealed class WslConfigPendingChange
{
    public WslConfigEntry ConfigEntry { get; init; }
    public required IWslConfigSetting CurrentSetting { get; init; }
    public required IWslConfigSetting PendingSetting { get; init; }
}

public interface IWslConfigSetting
{
    WslConfigEntry ConfigEntry { get; }
    string StringValue { get; }
    ulong UInt64Value { get; }
    int Int32Value { get; }
    bool BoolValue { get; }
    NetworkingConfiguration NetworkingConfigurationValue { get; }
    MemoryReclaimMode MemoryReclaimModeValue { get; }
    uint SetValue(object? value);
    bool Equals(object? obj);
}