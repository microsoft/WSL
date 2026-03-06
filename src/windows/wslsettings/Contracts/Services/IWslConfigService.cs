// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Contracts.Services;

public interface IWslConfigService
{
    IWslConfigSetting GetWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting = false);
    uint SetWslConfigSetting(IWslConfigSetting setting);
    bool HasPendingChanges { get; }
    IReadOnlyList<WslConfigPendingChange> GetPendingChanges();
    uint CommitPendingChanges();
    void ClearPendingChanges();
    public delegate void WslConfigChangedEventHandler();
    event WslConfigChangedEventHandler WslConfigChanged;
    public delegate void PendingChangesChangedEventHandler();
    event PendingChangesChangedEventHandler PendingChangesChanged;
}

public sealed class WslConfigPendingChange
{
    public WslConfigEntry ConfigEntry { get; init; }
    public object CurrentValue { get; init; } = string.Empty;
    public object PendingValue { get; init; } = string.Empty;
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