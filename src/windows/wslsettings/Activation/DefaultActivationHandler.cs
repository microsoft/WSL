// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using WslSettings.Contracts.Services;
using WslSettings.ViewModels.Settings;

namespace WslSettings.Activation;

public class DefaultActivationHandler : ActivationHandler<LaunchActivatedEventArgs>
{
    public DefaultActivationHandler(INavigationService navigationService) : base(navigationService)
    {
    }

    protected override bool CanHandleInternal(LaunchActivatedEventArgs args)
    {
        // None of the ActivationHandlers has handled the activation.
        return _navigationService.Frame?.Content == null;
    }

    protected async override Task HandleInternalAsync(LaunchActivatedEventArgs args)
    {
        Navigate(IWindowService.WindowId.MainWindow, typeof(MemAndProcViewModel).FullName!, args);

        await Task.CompletedTask;
    }
}
