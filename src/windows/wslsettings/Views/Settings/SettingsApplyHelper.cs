// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.ComponentModel;
using System.Diagnostics;
using WslSettings.Contracts.Services;

namespace WslSettings.Views.Settings;

internal static class SettingsApplyHelper
{
    public static async Task ShowApplyChangesDialogAsync(XamlRoot? xamlRoot)
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
            changeLines.Add($"- {GetSettingName(change.ConfigEntry)}: {FormatSettingValue(change.PendingSetting)}");
        }

        var contentText = string.Join(Environment.NewLine, changeLines);

        var contentPanel = new StackPanel { Spacing = 8 };
        contentPanel.Children.Add(new TextBlock
        {
            Text = "Settings_ApplyChangesDialogDescription".GetLocalized(),
            TextWrapping = TextWrapping.Wrap,
        });
        contentPanel.Children.Add(new TextBlock
        {
            Text = contentText,
            TextWrapping = TextWrapping.Wrap,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        });

        var dialog = new ContentDialog
        {
            XamlRoot = xamlRoot,
            Title = "Settings_ApplyChangesDialogTitle".GetLocalized(),
            Content = contentPanel,
            PrimaryButtonText = "Settings_ApplyChangesDialogShutdownButton".GetLocalized(),
            SecondaryButtonText = "Settings_ApplyChangesDialogLaterButton".GetLocalized(),
            DefaultButton = ContentDialogButton.Primary,
        };

        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.Primary)
        {
            // "Shutdown WSL now" — commit to disk and shutdown
            var commitResult = wslConfigService.CommitPendingChanges();
            if (commitResult != 0)
            {
                await ShowFailureDialogAsync(xamlRoot, string.Format("Settings_ApplyChangesDialogCommitFailed".GetLocalized(), commitResult));
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
            catch (Win32Exception ex)
            {
                await ShowFailureDialogAsync(xamlRoot, string.Format("Settings_ApplyChangesDialogShutdownFailed".GetLocalized(), ex.Message));
            }
            catch (InvalidOperationException ex)
            {
                await ShowFailureDialogAsync(xamlRoot, string.Format("Settings_ApplyChangesDialogShutdownFailed".GetLocalized(), ex.Message));
            }
        }
        else if (result == ContentDialogResult.Secondary)
        {
            // "Later" — commit to disk but don't shutdown; settings apply on next WSL restart
            var commitResult = wslConfigService.CommitPendingChanges();
            if (commitResult != 0)
            {
                await ShowFailureDialogAsync(xamlRoot, string.Format("Settings_ApplyChangesDialogCommitFailed".GetLocalized(), commitResult));
            }
        }
        // Esc / close — do nothing, leave changes pending
    }

    private static async Task ShowFailureDialogAsync(XamlRoot? xamlRoot, string message)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = xamlRoot,
            Title = "Settings_ApplyChangesDialogFailedTitle".GetLocalized(),
            Content = new TextBlock
            {
                Text = message,
                TextWrapping = TextWrapping.Wrap,
                MaxWidth = 620,
            },
            CloseButtonText = "Settings_ApplyChangesDialogCloseButton".GetLocalized(),
            DefaultButton = ContentDialogButton.Close,
        };

        await dialog.ShowAsync();
    }

    private static string GetSettingName(WslConfigEntry entry)
    {
        return entry switch
        {
            WslConfigEntry.ProcessorCount => "processors",
            WslConfigEntry.MemorySizeBytes => "memory",
            WslConfigEntry.SwapSizeBytes => "swap",
            WslConfigEntry.SwapFilePath => "swapFile",
            WslConfigEntry.VhdSizeBytes => "defaultVhdSize",
            WslConfigEntry.NetworkingMode => "networkingMode",
            WslConfigEntry.FirewallEnabled => "firewall",
            WslConfigEntry.IgnoredPorts => "ignoredPorts",
            WslConfigEntry.LocalhostForwardingEnabled => "localhostForwarding",
            WslConfigEntry.HostAddressLoopbackEnabled => "hostAddressLoopback",
            WslConfigEntry.AutoProxyEnabled => "autoProxy",
            WslConfigEntry.InitialAutoProxyTimeout => "initialAutoProxyTimeout",
            WslConfigEntry.DNSProxyEnabled => "dnsProxy",
            WslConfigEntry.DNSTunnelingEnabled => "dnsTunneling",
            WslConfigEntry.BestEffortDNSParsingEnabled => "bestEffortDnsParsing",
            WslConfigEntry.AutoMemoryReclaim => "autoMemoryReclaim",
            WslConfigEntry.GUIApplicationsEnabled => "guiApplications",
            WslConfigEntry.NestedVirtualizationEnabled => "nestedVirtualization",
            WslConfigEntry.SafeModeEnabled => "safeMode",
            WslConfigEntry.SparseVHDEnabled => "sparseVhd",
            WslConfigEntry.VMIdleTimeout => "vmIdleTimeout",
            WslConfigEntry.DebugConsoleEnabled => "debugConsole",
            WslConfigEntry.HardwarePerformanceCountersEnabled => "hardwarePerformanceCounters",
            WslConfigEntry.KernelPath => "kernel",
            WslConfigEntry.SystemDistroPath => "systemDistro",
            WslConfigEntry.KernelModulesPath => "kernelModules",
            _ => entry.ToString(),
        };
    }

    private static string FormatSettingValue(IWslConfigSetting setting)
    {
        switch (setting.ConfigEntry)
        {
            case WslConfigEntry.MemorySizeBytes:
            case WslConfigEntry.SwapSizeBytes:
            case WslConfigEntry.VhdSizeBytes:
                return $"{setting.UInt64Value / Constants.MB} MB";
            case WslConfigEntry.ProcessorCount:
                return setting.Int32Value.ToString();
            case WslConfigEntry.InitialAutoProxyTimeout:
            case WslConfigEntry.VMIdleTimeout:
                return $"{setting.Int32Value} ms";
            case WslConfigEntry.SwapFilePath:
            case WslConfigEntry.IgnoredPorts:
            case WslConfigEntry.KernelPath:
            case WslConfigEntry.SystemDistroPath:
            case WslConfigEntry.KernelModulesPath:
                return setting.StringValue;
            case WslConfigEntry.NetworkingMode:
                return setting.NetworkingConfigurationValue.ToString();
            case WslConfigEntry.AutoMemoryReclaim:
                return setting.MemoryReclaimModeValue.ToString();
            default:
                return setting.BoolValue ? "true" : "false";
        }
    }
}
