// Copyright (C) Microsoft Corporation. All rights reserved.

using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

using System.Diagnostics;

using Windows.System;

using WslSettings.Contracts.Services;
using WslSettings.Services;
using WslSettings.ViewModels;

namespace WslSettings.Views.Settings;

public sealed partial class ShellPage : Page, INotifyPropertyChanged
{
    private readonly Microsoft.UI.Dispatching.DispatcherQueue _dispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();

    public event PropertyChangedEventHandler? PropertyChanged;

    public bool HasPendingChanges => App.GetService<IWslConfigService>().HasPendingChanges;

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

        App.GetService<IWslConfigService>().PendingChangesChanged += OnPendingChangesChanged;

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

    private void OnPendingChangesChanged()
    {
        _dispatcherQueue.TryEnqueue(() =>
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HasPendingChanges)));
        });
    }

    private async void ApplyChanges_Click(object sender, RoutedEventArgs e)
    {
        var wslConfigService = App.GetService<IWslConfigService>();
        var pendingChanges = wslConfigService.GetPendingChanges();
        if (pendingChanges.Count == 0)
        {
            return;
        }

        var changeLines = new List<string>(pendingChanges.Count);
        foreach (var change in pendingChanges)
        {
            changeLines.Add($"- {FormatDisplayName(change.ConfigEntry)}: {FormatValue(change.ConfigEntry, change.PendingValue)}");
        }

        var contentPanel = new StackPanel { Spacing = 8 };
        contentPanel.Children.Add(new TextBlock
        {
            Text = "Settings_ApplyChangesDialogDescription".GetLocalized(),
            TextWrapping = TextWrapping.Wrap,
        });
        contentPanel.Children.Add(new TextBlock
        {
            Text = string.Join(Environment.NewLine, changeLines),
            TextWrapping = TextWrapping.Wrap,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        });

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Settings_ApplyChangesDialogTitle".GetLocalized(),
            Content = contentPanel,
            PrimaryButtonText = "Settings_ApplyChangesDialogShutdownButton".GetLocalized(),
            CloseButtonText = "Settings_ApplyChangesDialogCancelButton".GetLocalized(),
            DefaultButton = ContentDialogButton.Primary,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        var commitResult = wslConfigService.CommitPendingChanges();
        if (commitResult != 0)
        {
            await ShowErrorDialogAsync(string.Format("Settings_ApplyChangesDialogCommitFailed".GetLocalized(), commitResult));
            return;
        }

        try
        {
            var wslPath = Path.Combine(AppContext.BaseDirectory, "..", "wsl.exe");
            Process.Start(new ProcessStartInfo
            {
                FileName = wslPath,
                Arguments = "--shutdown",
                CreateNoWindow = true,
                UseShellExecute = false,
            })?.Dispose();
        }
        catch (Exception ex) when (ex is Win32Exception or InvalidOperationException)
        {
            await ShowErrorDialogAsync(string.Format("Settings_ApplyChangesDialogShutdownFailed".GetLocalized(), ex.Message));
        }
    }

    private async Task ShowErrorDialogAsync(string message)
    {
        await new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Settings_ApplyChangesDialogFailedTitle".GetLocalized(),
            Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap, MaxWidth = 620 },
            CloseButtonText = "Settings_ApplyChangesDialogCloseButton".GetLocalized(),
            DefaultButton = ContentDialogButton.Close,
        }.ShowAsync();
    }

    private static readonly IReadOnlyDictionary<WslConfigEntry, string> SettingDisplayNames =
        new Dictionary<WslConfigEntry, string>
        {
            { WslConfigEntry.ProcessorCount, "Settings_ProcCount/Header" },
            { WslConfigEntry.MemorySizeBytes, "Settings_MemorySize/Header" },
            { WslConfigEntry.SwapSizeBytes, "Settings_SwapSize/Header" },
            { WslConfigEntry.SwapFilePath, "Settings_SwapFilePath/Header" },
            { WslConfigEntry.VhdSizeBytes, "Settings_DefaultVHDSize/Header" },
            { WslConfigEntry.NetworkingMode, "Settings_NetworkingMode/Header" },
            { WslConfigEntry.FirewallEnabled, "Settings_HyperVFirewall/Header" },
            { WslConfigEntry.IgnoredPorts, "Settings_IgnoredPorts/Header" },
            { WslConfigEntry.LocalhostForwardingEnabled, "Settings_LocalhostForwarding/Header" },
            { WslConfigEntry.HostAddressLoopbackEnabled, "Settings_HostAddressLoopback/Header" },
            { WslConfigEntry.AutoProxyEnabled, "Settings_AutoProxy/Header" },
            { WslConfigEntry.InitialAutoProxyTimeout, "Settings_InitialAutoProxyTimeout/Header" },
            { WslConfigEntry.DNSProxyEnabled, "Settings_DNSProxy/Header" },
            { WslConfigEntry.DNSTunnelingEnabled, "Settings_DNSTunneling/Header" },
            { WslConfigEntry.BestEffortDNSParsingEnabled, "Settings_BestEffortDNS/Header" },
            { WslConfigEntry.AutoMemoryReclaim, "Settings_AutoMemoryReclaim/Header" },
            { WslConfigEntry.GUIApplicationsEnabled, "Settings_GUIApplications/Header" },
            { WslConfigEntry.NestedVirtualizationEnabled, "Settings_NestedVirtualization/Header" },
            { WslConfigEntry.SafeModeEnabled, "Settings_SafeMode/Header" },
            { WslConfigEntry.SparseVHDEnabled, "Settings_SparseVHD/Header" },
            { WslConfigEntry.VMIdleTimeout, "Settings_VMIdleTimeout/Header" },
            { WslConfigEntry.DebugConsoleEnabled, "Settings_DebugConsole/Header" },
            { WslConfigEntry.HardwarePerformanceCountersEnabled, "Settings_HWPerfCounters/Header" },
            { WslConfigEntry.KernelPath, "Settings_CustomKernelPath/Header" },
            { WslConfigEntry.SystemDistroPath, "Settings_CustomSystemDistroPath/Header" },
            { WslConfigEntry.KernelModulesPath, "Settings_CustomKernelModulesPath/Header" },
        };

    private static string FormatDisplayName(WslConfigEntry entry)
    {
        if (SettingDisplayNames.TryGetValue(entry, out var key))
        {
            var localized = key.GetLocalized();
            if (!string.IsNullOrEmpty(localized) && localized != key)
            {
                return localized;
            }
        }
        return entry.ToString();
    }

    private static string FormatValue(WslConfigEntry entry, object value)
    {
        switch (entry.GetValueKind())
        {
            case WslConfigValueKind.UInt64:
                return string.Format("Settings_MegabyteStringFormat".GetLocalized(), (ulong)value / Constants.MB);
            case WslConfigValueKind.Int32:
                return entry is WslConfigEntry.InitialAutoProxyTimeout or WslConfigEntry.VMIdleTimeout
                    ? string.Format("Settings_MillisecondsStringFormat".GetLocalized(), (int)value)
                    : ((int)value).ToString();
            case WslConfigValueKind.String:
                return (string?)value ?? string.Empty;
            case WslConfigValueKind.Bool:
                var boolText = (bool)value ? "Settings_BooleanTrueText".GetLocalized() : "Settings_BooleanFalseText".GetLocalized();
                return string.IsNullOrEmpty(boolText) ? value.ToString()! : boolText;
            default:
                try
                {
                    var resourceKey = $"Settings_{value.GetType().Name}_{value}";
                    var localized = resourceKey.GetLocalized();
                    if (!string.IsNullOrEmpty(localized) && localized != resourceKey)
                    {
                        return localized;
                    }
                }
                catch (COMException) { }
                return value.ToString()!;
        }
    }
}
