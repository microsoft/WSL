// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Contracts.Services;

public interface IWslConfigService
{
    IWslConfigSetting GetWslConfigSetting(WslConfigEntry wslConfigEntry, bool defaultSetting = false);
    uint SetWslConfigSetting(IWslConfigSetting setting);
    public delegate void WslConfigChangedEventHandler();
    event WslConfigChangedEventHandler WslConfigChanged;
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