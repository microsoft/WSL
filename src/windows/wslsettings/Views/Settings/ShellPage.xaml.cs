// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

using System.Diagnostics;

using Windows.System;

using WslSettings.Contracts.Services;
using WslSettings.Services;
using WslSettings.ViewModels;

namespace WslSettings.Views.Settings;

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
        App.MainWindow!.ExtendsContentIntoTitleBar = true;
        App.MainWindow.SetTitleBar(AppTitleBar);
        App.MainWindow.Activated += MainWindow_Activated;
        AppTitleBarText.Text = "Settings_AppDisplayName".GetLocalized();
        NavigationFrame.LostFocus += NavigationFrame_LostFocus;
    }

    private void OnLoaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        TitleBarHelper.UpdateTitleBar(App.MainWindow!, RequestedTheme);

        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.Left, VirtualKeyModifiers.Menu));
        KeyboardAccelerators.Add(BuildKeyboardAccelerator(VirtualKey.GoBack));
    }

    private void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        App.AppTitlebar = AppTitleBarText as UIElement;
        RegisterNavigationService();
        if (ViewModel.NavigationService.Frame?.Content == null)
        {
            ViewModel.NavigationService.NavigateTo(typeof(ViewModels.Settings.MemAndProcViewModel).FullName!);
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
            case "LaunchWSL":
                var wslPath = Path.Combine(AppContext.BaseDirectory, "..", "wsl.exe");
                await Task.Run(() => Process.Start(wslPath, "--cd ~"));
                break;
            case "OOBE":
                await Task.Run(() =>
                {
                    _dispatcherQueue.TryEnqueue(() =>
                    {
                        App.GetService<IWindowService>().CreateOrGetWindow(IWindowService.WindowId.OOBEWindow).Activate();
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

    private void NavigationFrame_LostFocus(object sender, RoutedEventArgs e)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            var focused = FocusManager.GetFocusedElement(NavigationViewControl.XamlRoot);

            // If focus is transferred to the selected page itself, do nothing
            if (focused is not NavigationViewItem && focused is not NavigationView)
            {
                return;
            }

            // Restore focus to the selected navigation item
            var selected = NavigationViewControl.SelectedItem;
            var container = NavigationViewControl.ContainerFromMenuItem(selected) as Control;
            container?.Focus(FocusState.Keyboard);
        });
    }
}
