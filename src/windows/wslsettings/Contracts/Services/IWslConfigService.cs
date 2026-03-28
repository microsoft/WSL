// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Contracts.Services;

public interface IWslConfigService
{
    IWslConfigSetting GetWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting = false);

    /// <summary>
    /// Stages a config value change in memory. The value is only written to disk when
    /// <see cref="CommitPendingChanges"/> is called.
    /// </summary>
    uint SetWslConfigSetting(IWslConfigSetting setting);

    /// <summary>
    /// True when there are in-app changes that have not yet been committed to disk.
    /// </summary>
    bool HasPendingChanges { get; }

    /// <summary>
    /// Returns pending changes as (entry, new value) pairs for UI display.
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
    public required object PendingValue { get; init; }
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

/// <summary>
/// Describes which union field a <see cref="WslConfigEntry"/> uses inside the native WslConfigSetting struct.
/// </summary>
public enum WslConfigValueKind
{
    Bool,
    Int32,
    UInt64,
    String,
    NetworkingConfiguration,
    MemoryReclaimMode,
}

public static class WslConfigEntryExtensions
{
    /// <summary>
    /// Returns the <see cref="WslConfigValueKind"/> that describes which union field this entry uses.
    /// Keep this in sync with the native WslConfigSetting union in WslCoreConfigInterface.h.
    /// </summary>
    public static WslConfigValueKind GetValueKind(this WslConfigEntry entry) => entry switch
    {
        WslConfigEntry.SwapFilePath or
        WslConfigEntry.IgnoredPorts or
        WslConfigEntry.KernelPath or
        WslConfigEntry.SystemDistroPath or
        WslConfigEntry.KernelModulesPath => WslConfigValueKind.String,

        WslConfigEntry.ProcessorCount or
        WslConfigEntry.InitialAutoProxyTimeout or
        WslConfigEntry.VMIdleTimeout => WslConfigValueKind.Int32,

        WslConfigEntry.MemorySizeBytes or
        WslConfigEntry.SwapSizeBytes or
        WslConfigEntry.VhdSizeBytes => WslConfigValueKind.UInt64,

        WslConfigEntry.NetworkingMode => WslConfigValueKind.NetworkingConfiguration,

        WslConfigEntry.AutoMemoryReclaim => WslConfigValueKind.MemoryReclaimMode,

        _ => WslConfigValueKind.Bool,
    };
}