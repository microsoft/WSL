// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Windows.Input;
using WslSettings.Contracts.Services;

namespace WslSettings.ViewModels.Settings;

public partial class NetworkingViewModel : WslConfigSettingViewModel
{
    private IWslConfigSetting? _networkingMode;
    private IWslConfigSetting? _hyperVFirewall;
    private IWslConfigSetting? _ignoredPorts;
    private IWslConfigSetting? _localhostForwarding;
    private IWslConfigSetting? _hostAddressLoopback;
    private IWslConfigSetting? _autoProxy;
    private IWslConfigSetting? _initialAutoProxyTimeout;
    private IWslConfigSetting? _dNSProxy;
    private IWslConfigSetting? _dNSTunneling;
    private IWslConfigSetting? _bestEffortDNS;
    private string? _defaultIgnoredPorts;
    private int _defaultInitialAutoProxyTimeout;
    private List<ComboBoxItem> _networkingModeItems;
    private bool _ignoredPorts_ResetEnabled;
    private bool _initialAutoProxyTimeout_ResetEnabled;

    public NetworkingViewModel()
    {
        InitializeConfigSettings();

        InitialAutoProxyTimeout_ResetEnabled = !Equals(_defaultInitialAutoProxyTimeout, _initialAutoProxyTimeout!.Int32Value);

        _networkingModeItems = new List<ComboBoxItem>();
        foreach (var networkModeItem in Enum.GetNames(typeof(NetworkingConfiguration)).ToList())
        {
            _networkingModeItems.Add(new ComboBoxItem() { Name = networkModeItem, Content = networkModeItem });
        }

        _networkingModeItems[(int)NetworkingConfiguration.Bridged].Visibility = NetworkingModeSelected == (int)NetworkingConfiguration.Bridged ?
            Visibility.Visible : Visibility.Collapsed;
    }

    protected override void InitializeConfigSettings()
    {
        var wslConfigService = App.GetService<IWslConfigService>();
        _networkingMode = wslConfigService.GetWslConfigSetting(WslConfigEntry.NetworkingMode);
        _hyperVFirewall = wslConfigService.GetWslConfigSetting(WslConfigEntry.FirewallEnabled);
        _ignoredPorts = wslConfigService.GetWslConfigSetting(WslConfigEntry.IgnoredPorts);
        _localhostForwarding = wslConfigService.GetWslConfigSetting(WslConfigEntry.LocalhostForwardingEnabled);
        _hostAddressLoopback = wslConfigService.GetWslConfigSetting(WslConfigEntry.HostAddressLoopbackEnabled);
        _autoProxy = wslConfigService.GetWslConfigSetting(WslConfigEntry.AutoProxyEnabled);
        _initialAutoProxyTimeout = wslConfigService.GetWslConfigSetting(WslConfigEntry.InitialAutoProxyTimeout);
        _dNSProxy = wslConfigService.GetWslConfigSetting(WslConfigEntry.DNSProxyEnabled);
        _dNSTunneling = wslConfigService.GetWslConfigSetting(WslConfigEntry.DNSTunnelingEnabled);
        _bestEffortDNS = wslConfigService.GetWslConfigSetting(WslConfigEntry.BestEffortDNSParsingEnabled);

        string defaultIgnoredPorts = wslConfigService.GetWslConfigSetting(WslConfigEntry.IgnoredPorts, true).StringValue;
        _defaultIgnoredPorts = defaultIgnoredPorts == null ? String.Empty : defaultIgnoredPorts;
        _defaultInitialAutoProxyTimeout = wslConfigService.GetWslConfigSetting(WslConfigEntry.InitialAutoProxyTimeout, true).Int32Value;
    }

    public List<ComboBoxItem> NetworkingModes
    {
        get { return _networkingModeItems; }
    }

    public int NetworkingModeSelected
    {
        get { return (int)_networkingMode!.NetworkingConfigurationValue; }
        set { Set(ref _networkingMode!, value); }
    }

    public bool IsOnHyperVFirewall
    {
        get { return _hyperVFirewall!.BoolValue; }
        set { Set(ref _hyperVFirewall!, value); }
    }

    public string IgnoredPorts
    {
        get
        {
            IgnoredPorts_ResetEnabled = !Equals(_defaultIgnoredPorts, _ignoredPorts!.StringValue);
            return _ignoredPorts!.StringValue;
        }
        set
        {
            if (ValidateInput(value, Constants.CommaSeparatedWholeNumbersOrEmptyRegex))
            {
                Set(ref _ignoredPorts!, value);
            }
        }
    }

    public bool IgnoredPorts_ResetEnabled
    {
        get => _ignoredPorts_ResetEnabled;
        set => SetProperty(ref _ignoredPorts_ResetEnabled, value);
    }

    private void IgnoredPorts_ResetExecuted(string? param)
    {
        IgnoredPorts = _defaultIgnoredPorts!;
    }

    public ICommand IgnoredPorts_ResetCommand => new RelayCommand<string>(IgnoredPorts_ResetExecuted);

    public bool IsOnLocalhostForwarding
    {
        get { return _localhostForwarding!.BoolValue; }
        set { Set(ref _localhostForwarding!, value); }
    }

    public bool IsOnHostAddressLoopback
    {
        get { return _hostAddressLoopback!.BoolValue; }
        set { Set(ref _hostAddressLoopback!, value); }
    }

    public bool IsOnAutoProxy
    {
        get { return _autoProxy!.BoolValue; }
        set { Set(ref _autoProxy!, value); }
    }

    public string InitialAutoProxyTimeout
    {
        get
        {
            return _initialAutoProxyTimeout!.Int32Value.ToString();
        }
        set
        {
            if (ValidateInput(value, Constants.IntegerRegex))
            {
                Set(ref _initialAutoProxyTimeout!, Convert.ToInt32(value));
            }
        }
    }

    public void SetInitialAutoProxyTimeout_ResetEnabled(string? value)
    {
        if (Int32.TryParse(value, out Int32 parseResult))
        {
            InitialAutoProxyTimeout_ResetEnabled = !Equals(_defaultInitialAutoProxyTimeout, parseResult);
        }
        else
        {
            InitialAutoProxyTimeout_ResetEnabled = true;
        }
    }

    public bool InitialAutoProxyTimeout_ResetEnabled
    {
        get => _initialAutoProxyTimeout_ResetEnabled;
        set => SetProperty(ref _initialAutoProxyTimeout_ResetEnabled, value);
    }

    private void InitialAutoProxyTimeout_ResetExecuted(string? param)
    {
        InitialAutoProxyTimeout = _defaultInitialAutoProxyTimeout.ToString();
    }

    public ICommand InitialAutoProxyTimeout_ResetCommand => new RelayCommand<string>(InitialAutoProxyTimeout_ResetExecuted);

    public bool IsOnDNSProxy
    {
        get { return _dNSProxy!.BoolValue; }
        set { Set(ref _dNSProxy!, value); }
    }

    public bool IsOnDNSTunneling
    {
        get { return _dNSTunneling!.BoolValue; }
        set { Set(ref _dNSTunneling!, value); }
    }

    public bool IsOnBestEffortDNS
    {
        get { return _bestEffortDNS!.BoolValue; }
        set { Set(ref _bestEffortDNS!, value); }
    }
}