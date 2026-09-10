// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using System.Windows.Input;
using WslSettings.Contracts.Services;

namespace WslSettings.ViewModels.Settings;

public partial class OptionalFeaturesViewModel : WslConfigSettingViewModel
{
    private IWslConfigSetting? _memoryReclaimMode;
    private IWslConfigSetting? _gUIApplications;
    private IWslConfigSetting? _nestedVirtualization;
    private IWslConfigSetting? _safeMode;
    private IWslConfigSetting? _sparseVHD;
    private IWslConfigSetting? _vMIdleTimeout;
    private int _defaultVMIdleTimeout;
    private IWslConfigSetting? _instanceIdleTimeout;
    private int _defaultInstanceIdleTimeout;
    private int _previousInstanceIdleTimeout;

    public OptionalFeaturesViewModel()
    {
        InitializeConfigSettings();

        VMIdleTimeout_ResetEnabled = !Equals(_defaultVMIdleTimeout, _vMIdleTimeout!.Int32Value);
        InstanceIdleTimeout_ResetEnabled = !Equals(_defaultInstanceIdleTimeout, _instanceIdleTimeout!.Int32Value);
    }

    protected override void InitializeConfigSettings()
    {
        var wslConfigService = App.GetService<IWslConfigService>();
        _memoryReclaimMode = wslConfigService.GetWslConfigSetting(WslConfigEntry.AutoMemoryReclaim);
        _gUIApplications = wslConfigService.GetWslConfigSetting(WslConfigEntry.GUIApplicationsEnabled);
        _nestedVirtualization = wslConfigService.GetWslConfigSetting(WslConfigEntry.NestedVirtualizationEnabled);
        _safeMode = wslConfigService.GetWslConfigSetting(WslConfigEntry.SafeModeEnabled);
        _sparseVHD = wslConfigService.GetWslConfigSetting(WslConfigEntry.SparseVHDEnabled);
        _vMIdleTimeout = wslConfigService.GetWslConfigSetting(WslConfigEntry.VMIdleTimeout);
        _instanceIdleTimeout = wslConfigService.GetWslConfigSetting(WslConfigEntry.InstanceIdleTimeout);

        _defaultVMIdleTimeout = wslConfigService.GetWslConfigSetting(WslConfigEntry.VMIdleTimeout, true).Int32Value;
        _defaultInstanceIdleTimeout = wslConfigService.GetWslConfigSetting(WslConfigEntry.InstanceIdleTimeout, true).Int32Value;

        var configuredInstanceIdleTimeout = _instanceIdleTimeout!.Int32Value;
        _previousInstanceIdleTimeout = configuredInstanceIdleTimeout < 0 ? _defaultInstanceIdleTimeout : configuredInstanceIdleTimeout;
    }

    public List<string> MemoryReclaimModes
    {
        get { return Enum.GetNames(typeof(MemoryReclaimMode)).ToList(); }
    }

    public int MemoryReclaimModeSelected
    {
        get { return (int)_memoryReclaimMode!.MemoryReclaimModeValue; }
        set { Set(ref _memoryReclaimMode!, value); }
    }

    public bool IsOnGUIApplications
    {
        get { return _gUIApplications!.BoolValue; }
        set { Set(ref _gUIApplications!, value); }
    }

    public bool IsOnNestedVirtualization
    {
        get { return _nestedVirtualization!.BoolValue; }
        set { Set(ref _nestedVirtualization!, value); }
    }

    public bool IsOnSafeMode
    {
        get { return _safeMode!.BoolValue; }
        set { Set(ref _safeMode!, value); }
    }

    public bool IsOnSparseVHD
    {
        get { return _sparseVHD!.BoolValue; }
        set { Set(ref _sparseVHD!, value); }
    }

    // Keeping WSL running is driven by the distribution idle timeout: a negative value tells the
    // service to never idle-terminate a distribution, and because the VM is only considered idle
    // once every distribution has stopped, this keeps the WSL VM running as well.
    public bool IsOnKeepWslRunning
    {
        get { return _instanceIdleTimeout!.Int32Value < 0; }
        set
        {
            if (value)
            {
                var currentInstanceIdleTimeout = _instanceIdleTimeout!.Int32Value;
                if (currentInstanceIdleTimeout >= 0)
                {
                    _previousInstanceIdleTimeout = currentInstanceIdleTimeout;
                }

                Set(ref _instanceIdleTimeout!, -1, nameof(IsOnKeepWslRunning));
            }
            else
            {
                Set(ref _instanceIdleTimeout!, _previousInstanceIdleTimeout, nameof(IsOnKeepWslRunning));
            }

            OnPropertyChanged(nameof(InstanceIdleTimeout));
            OnPropertyChanged(nameof(InstanceIdleTimeoutEnabled));
            OnPropertyChanged(nameof(VMIdleTimeoutEnabled));
            InstanceIdleTimeout_ResetEnabled = !Equals(_defaultInstanceIdleTimeout, _instanceIdleTimeout!.Int32Value);
        }
    }

    public bool VMIdleTimeoutEnabled
    {
        get { return !IsOnKeepWslRunning; }
    }

    public bool InstanceIdleTimeoutEnabled
    {
        get { return !IsOnKeepWslRunning; }
    }

    public string VMIdleTimeout
    {
        get
        {
            return _vMIdleTimeout!.Int32Value.ToString();
        }
        set
        {
            if (ValidateInput(value, Constants.IntegerRegex))
            {
                if (Int32.TryParse(value, out int parsedValue))
                {
                    Set(ref _vMIdleTimeout!, parsedValue);
                }
                else
                {
                    OnPropertyChanged();
                }
            }
        }
    }

    public void SetVMIdleTimeout_ResetEnabled(string? value)
    {
        if (Int32.TryParse(value, out Int32 parseResult))
        {
            VMIdleTimeout_ResetEnabled = !Equals(_defaultVMIdleTimeout, parseResult);
        }
        else
        {
            VMIdleTimeout_ResetEnabled = true;
        }
    }

    private bool _vMIdleTimeout_ResetEnabled;

    public bool VMIdleTimeout_ResetEnabled
    {
        get => _vMIdleTimeout_ResetEnabled;
        set => SetProperty(ref _vMIdleTimeout_ResetEnabled, value);
    }

    private void VMIdleTimeout_ResetExecuted(string? param)
    {
        VMIdleTimeout = _defaultVMIdleTimeout.ToString();
    }

    public ICommand VMIdleTimeout_ResetCommand => new RelayCommand<string>(VMIdleTimeout_ResetExecuted);

    public string InstanceIdleTimeout
    {
        get
        {
            return _instanceIdleTimeout!.Int32Value.ToString();
        }
        set
        {
            if (ValidateInput(value, Constants.IntegerRegex))
            {
                if (Int32.TryParse(value, out int parsedValue))
                {
                    Set(ref _instanceIdleTimeout!, parsedValue);
                    if (parsedValue >= 0)
                    {
                        _previousInstanceIdleTimeout = parsedValue;
                    }

                    OnPropertyChanged(nameof(IsOnKeepWslRunning));
                    OnPropertyChanged(nameof(InstanceIdleTimeoutEnabled));
                    OnPropertyChanged(nameof(VMIdleTimeoutEnabled));
                }
                else
                {
                    OnPropertyChanged();
                }
            }
        }
    }

    public void SetInstanceIdleTimeout_ResetEnabled(string? value)
    {
        if (Int32.TryParse(value, out Int32 parseResult))
        {
            InstanceIdleTimeout_ResetEnabled = !Equals(_defaultInstanceIdleTimeout, parseResult);
        }
        else
        {
            InstanceIdleTimeout_ResetEnabled = true;
        }
    }

    private bool _instanceIdleTimeout_ResetEnabled;

    public bool InstanceIdleTimeout_ResetEnabled
    {
        get => _instanceIdleTimeout_ResetEnabled;
        set => SetProperty(ref _instanceIdleTimeout_ResetEnabled, value);
    }

    private void InstanceIdleTimeout_ResetExecuted(string? param)
    {
        InstanceIdleTimeout = _defaultInstanceIdleTimeout.ToString();
    }

    public ICommand InstanceIdleTimeout_ResetCommand => new RelayCommand<string>(InstanceIdleTimeout_ResetExecuted);
}