// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using System.Windows.Input;
using WslSettings.Contracts.Services;

namespace WslSettings.ViewModels.Settings;

public partial class MemAndProcViewModel : WslConfigSettingViewModel
{
    private IWslConfigSetting? _procCount;
    private IWslConfigSetting? _memorySize;
    private IWslConfigSetting? _swapSize;
    private IWslConfigSetting? _swapFilePath;
    private int _defaultProcCount;
    private ulong _defaultMemorySize;
    private ulong _defaultSwapSize;
    private bool _procCount_ResetEnabled;
    private bool _memorySize_ResetEnabled;
    private bool _swapSize_ResetEnabled;

    public MemAndProcViewModel()
    {
        InitializeConfigSettings();

        ProcCount_ResetEnabled = !Equals(_defaultProcCount, _procCount!.Int32Value);
        MemorySize_ResetEnabled = !Equals(_defaultMemorySize, _memorySize!.UInt64Value);
        SwapSize_ResetEnabled = !Equals(_defaultSwapSize, _swapSize!.UInt64Value);
    }

    protected override void InitializeConfigSettings()
    {
        var wslConfigService = App.GetService<IWslConfigService>();
        _procCount = wslConfigService.GetWslConfigSetting(WslConfigEntry.ProcessorCount);
        _memorySize = wslConfigService.GetWslConfigSetting(WslConfigEntry.MemorySizeBytes);
        _swapSize = wslConfigService.GetWslConfigSetting(WslConfigEntry.SwapSizeBytes);
        _swapFilePath = wslConfigService.GetWslConfigSetting(WslConfigEntry.SwapFilePath);

        _defaultProcCount = wslConfigService.GetWslConfigSetting(WslConfigEntry.ProcessorCount, true).Int32Value;
        _defaultMemorySize = wslConfigService.GetWslConfigSetting(WslConfigEntry.MemorySizeBytes, true).UInt64Value;
        _defaultSwapSize = wslConfigService.GetWslConfigSetting(WslConfigEntry.SwapSizeBytes, true).UInt64Value;
    }

    public string ProcCount
    {
        get
        {
            return _procCount!.Int32Value.ToString();
        }
        set
        {
            if (ValidateInput(value, Constants.IntegerRegex))
            {
                Set(ref _procCount!, Convert.ToInt32(value));
            }
        }
    }

    public void SetProcCount_ResetEnabled(string? value)
    {
        if (Int32.TryParse(value, out Int32 parseResult))
        {
            ProcCount_ResetEnabled = !Equals(_defaultProcCount, parseResult);
        }
        else
        {
            ProcCount_ResetEnabled = true;
        }
    }

    public bool ProcCount_ResetEnabled
    {
        get => _procCount_ResetEnabled;
        set => SetProperty(ref _procCount_ResetEnabled, value);
    }

    private void ProcCount_ResetExecuted(string? param)
    {
        ProcCount = _defaultProcCount.ToString();
    }

    public ICommand ProcCount_ResetCommand => new RelayCommand<string>(ProcCount_ResetExecuted);

    public string MemorySize
    {
        get
        {
            return _memorySize!.UInt64Value.ToString();
        }
        set
        {
            if (ValidateInput(value, Constants.WholeNumberRegex))
            {
                Set(ref _memorySize!, Convert.ToUInt64(value));
            }
        }
    }

    public void SetMemorySize_ResetEnabled(string? value)
    {
        if (UInt64.TryParse(value, out UInt64 parseResult))
        {
            MemorySize_ResetEnabled = !Equals(_defaultMemorySize / Constants.MB, parseResult);
        }
        else
        {
            MemorySize_ResetEnabled = true;
        }
    }

    public bool MemorySize_ResetEnabled
    {
        get => _memorySize_ResetEnabled;
        set => SetProperty(ref _memorySize_ResetEnabled, value);
    }

    private void MemorySize_ResetExecuted(string? param)
    {
        MemorySize = _defaultMemorySize.ToString();
    }

    public ICommand MemorySize_ResetCommand => new RelayCommand<string>(MemorySize_ResetExecuted);

    public string SwapSize
    {
        get
        {
            return _swapSize!.UInt64Value.ToString();
        }
        set
        {
            if (ValidateInput(value, Constants.WholeNumberRegex))
            {
                Set(ref _swapSize!, Convert.ToUInt64(value));
            }
        }
    }

    public void SetSwapSize_ResetEnabled(string? value)
    {
        if (UInt64.TryParse(value, out UInt64 parseResult))
        {
            SwapSize_ResetEnabled = !Equals(_defaultSwapSize / Constants.MB, parseResult);
        }
        else
        {
            SwapSize_ResetEnabled = true;
        }
    }

    public bool SwapSize_ResetEnabled
    {
        get => _swapSize_ResetEnabled;
        set => SetProperty(ref _swapSize_ResetEnabled, value);
    }

    private void SwapSize_ResetExecuted(string? param)
    {
        SwapSize = _defaultSwapSize.ToString();
    }

    public ICommand SwapSize_ResetCommand => new RelayCommand<string>(SwapSize_ResetExecuted);

    public string SwapFilePath
    {
        get { return _swapFilePath!.StringValue; }
        set { Set(ref _swapFilePath!, value); }
    }
}