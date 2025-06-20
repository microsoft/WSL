// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using System.Windows.Input;
using WslSettings.Contracts.Services;

namespace WslSettings.ViewModels.Settings;

public partial class FileSystemViewModel : WslConfigSettingViewModel
{
    private IWslConfigSetting? _defaultVHDSize;
    private ulong _defaultDefaultVHDSizeBytes;
    private bool _defaultVHDSize_ResetEnabled;

    public FileSystemViewModel()
    {
        InitializeConfigSettings();

        DefaultVHDSize_ResetEnabled = !Equals(_defaultDefaultVHDSizeBytes, _defaultVHDSize!.UInt64Value);
    }

    override protected void InitializeConfigSettings()
    {
        var wslConfigService = App.GetService<IWslConfigService>();
        _defaultVHDSize = wslConfigService.GetWslConfigSetting(WslConfigEntry.VhdSizeBytes);

        _defaultDefaultVHDSizeBytes = wslConfigService.GetWslConfigSetting(WslConfigEntry.VhdSizeBytes, true).UInt64Value;
    }

    public string DefaultVHDSize
    {
        get
        {
            return _defaultVHDSize!.UInt64Value.ToString();
        }
        set
        {
            if (ValidateInput(value, Constants.WholeNumberRegex))
            {
                Set(ref _defaultVHDSize!, Convert.ToUInt64(value));
            }
        }
    }

    public void SetDefaultVHDSize_ResetEnabled(string? value)
    {
        if (UInt64.TryParse(value, out UInt64 parseResult))
        {
            DefaultVHDSize_ResetEnabled = !Equals(_defaultDefaultVHDSizeBytes / Constants.MB, parseResult);
        }
        else
        {
            DefaultVHDSize_ResetEnabled = true;
        }
    }

    public bool DefaultVHDSize_ResetEnabled
    {
        get => _defaultVHDSize_ResetEnabled;
        set => SetProperty(ref _defaultVHDSize_ResetEnabled, value);
    }

    private void DefaultVHDSize_ResetExecuted(string? param)
    {
        DefaultVHDSize = _defaultDefaultVHDSizeBytes.ToString();
    }

    public ICommand DefaultVHDSize_ResetCommand => new RelayCommand<string>(DefaultVHDSize_ResetExecuted);
}