// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.UI.Dispatching;
using WslSettings.Contracts.Services;

namespace WslSettings.ViewModels.OOBE;

/// <summary>
/// The keep-WSL-running setting is saved to .wslconfig immediately and applies in the next WSL session.
/// </summary>
public partial class GeneralViewModel : ObservableRecipient
{
    private readonly DispatcherQueue _dispatcherQueue = DispatcherQueue.GetForCurrentThread();
    private readonly IWslConfigService _wslConfigService;

    public GeneralViewModel(IWslConfigService wslConfigService)
    {
        _wslConfigService = wslConfigService;
        _wslConfigService.WslConfigChanged += OnConfigChanged;
    }

    public void Unsubscribe()
    {
        _wslConfigService.WslConfigChanged -= OnConfigChanged;
    }

    public void OnConfigChanged()
    {
        _dispatcherQueue.TryEnqueue(() => OnPropertyChanged(string.Empty));
    }

    // A negative distribution idle timeout tells the service to never idle-terminate a distribution, and because the VM
    // is only considered idle once every distribution has stopped, this keeps the WSL VM running as well.
    // Reads the saved value rather than a staged one: this setting saves immediately, so it must reflect what's on disk.
    public bool IsKeepWslRunning
    {
        get => _wslConfigService.GetSavedWslConfigSetting(WslConfigEntry.InstanceIdleTimeout).Int32Value < 0;
        set
        {
            if (value != IsKeepWslRunning)
            {
                HasError = !SetKeepWslRunning(value);
                RefreshStatus();
            }
        }
    }

    private bool _hasError;
    public bool HasError
    {
        get => _hasError;
        set => SetProperty(ref _hasError, value);
    }

    private bool SetKeepWslRunning(bool enable)
    {
        // Always commit (even if the value looks unchanged) so a conflicting staged change for this entry is discarded.
        var current = _wslConfigService.GetSavedWslConfigSetting(WslConfigEntry.InstanceIdleTimeout).Int32Value;
        int value = enable ? -1 : (current >= 0 ? current : _wslConfigService.GetWslConfigSetting(WslConfigEntry.InstanceIdleTimeout, true).Int32Value);
        return _wslConfigService.CommitWslConfigSetting(WslConfigEntry.InstanceIdleTimeout, value) == 0;
    }

    // Deferred so the binding that triggered a setter has finished updating before everything is re-read
    // (this also reverts a toggle whose change failed to save).
    private void RefreshStatus()
    {
        _dispatcherQueue.TryEnqueue(() => OnPropertyChanged(string.Empty));
    }
}