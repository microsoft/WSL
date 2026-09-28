// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

using Windows.System;

using WslSettings.Contracts.Services;
using WslSettings.ViewModels;

namespace WslSettings.Views.OOBE;

public sealed partial class ShellPage : Page
{
    private readonly Microsoft.UI.Dispatching.DispatcherQueue _dispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();

    private void RegisterNavigationService()
    {
        ViewModel.NavigationViewService.UnregisterEvents();
        ViewModel.NavigationService.Frame = NavigationFrame;
        ViewModel.NavigationViewService.Initialize(NavigationViewControl);
    }

    public ShellViewModel ViewModel
    {
        get;
    }

    public ShellPage(ShellViewModel viewModel)
    {
        ViewModel = viewModel;
        InitializeComponent();

        RegisterNavigationService();

        // TODO: Set the title bar icon by updating /Assets/wsl.ico.
        // A custom title bar is required for full window theme and Mica support.
        // https://docs.microsoft.com/windows/apps/develop/title-bar?tabs=winui3#full-customization
        App.OOBEWindow!.ExtendsContentIntoTitleBar = true;
        App.OOBEWindow.SetTitleBar(AppTitleBar);
        App.OOBEWindow.Activated += OOBEWindow_Activated;
        AppTitleBarText.Text = "Settings_OOBEDisplayName".GetLocalized();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        TitleBarHelper.UpdateTitleBar(App.OOBEWindow!, RequestedTheme);

        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.Left, VirtualKeyModifiers.Menu));
        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.GoBack));
    }

    private void OOBEWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        App.AppTitlebar = AppTitleBarText as UIElement;
        RegisterNavigationService();
        if (ViewModel.NavigationService.Frame?.Content == null)
        {
            ViewModel.NavigationService.NavigateTo(typeof(ViewModels.OOBE.GeneralViewModel).FullName!);
        }
    }

    private void NavigationViewControl_DisplayModeChanged(NavigationView sender, NavigationViewDisplayModeChangedEventArgs args)
    {
        if (args.DisplayMode == NavigationViewDisplayMode.Compact || args.DisplayMode == NavigationViewDisplayMode.Minimal)
        {
            PaneToggleBtn.Visibility = Visibility.Visible;
            AppTitleBar.Margin = new Thickness(48, 0, 0, 0);
            AppTitleBarText.Margin = new Thickness(12, 0, 0, 0);
        }
        else
        {
            PaneToggleBtn.Visibility = Visibility.Collapsed;
            AppTitleBar.Margin = new Thickness(16, 0, 0, 0);
            AppTitleBarText.Margin = new Thickness(16, 0, 0, 0);
        }
    }

    private void PaneToggleBtn_Click(object sender, RoutedEventArgs e)
    {
        NavigationViewControl.IsPaneOpen = !NavigationViewControl.IsPaneOpen;
    }

    private async void NavigationViewControl_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
    {
        switch (args.InvokedItemContainer.Tag)
        {
            case "Settings":
                await Task.Run(() =>
                {
                    _dispatcherQueue.TryEnqueue(() =>
                    {
                        App.GetService<IWindowService>().CreateOrGetWindow(IWindowService.WindowId.MainWindow).Activate();
                    });
                });
                break;
            default:
                break;
        }
    }

    private static KeyboardAccelerator BuildKeyboardAccelerator(VirtualKey key, VirtualKeyModifiers? modifiers = null)
    {
        var keyboardAccelerator = new KeyboardAccelerator() { Key = key };

        if (modifiers.HasValue)
        {
            keyboardAccelerator.Modifiers = modifiers.Value;
        }

        keyboardAccelerator.Invoked += OnKeyboardAcceleratorInvoked;

        return keyboardAccelerator;
    }

    private static void OnKeyboardAcceleratorInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
    {
        var navigationService = App.GetService<INavigationService>();

        var result = navigationService.GoBack();

        args.Handled = result;
    }
}
