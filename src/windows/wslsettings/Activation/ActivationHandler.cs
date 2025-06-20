// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using WslSettings.Contracts.Services;

namespace WslSettings.Activation;

// Extend this class to implement new ActivationHandlers. See DefaultActivationHandler for an example.
// https://github.com/microsoft/TemplateStudio/blob/main/docs/WinUI/activation.md
public abstract class ActivationHandler<T> : IActivationHandler
    where T : class
{
    protected readonly INavigationService _navigationService;

    public ActivationHandler(INavigationService navigationService)
    {
        _navigationService = navigationService;
    }

    // Override this method to add the logic for whether to handle the activation.
    protected virtual bool CanHandleInternal(T args) => true;

    // Override this method to add the logic for your activation handler.
    protected abstract Task HandleInternalAsync(T args);

    protected void Navigate(IWindowService.WindowId windowId, string pageKey, LaunchActivatedEventArgs args)
    {
        WindowEx window = App.GetService<IWindowService>().CreateOrGetWindow(windowId);

        _navigationService.NavigateTo(pageKey, args.Arguments);

        window.Activate();
    }

    public bool CanHandle(object args) => args is T && CanHandleInternal((args as T)!);

    public async Task HandleAsync(object args) => await HandleInternalAsync((args as T)!);
}
