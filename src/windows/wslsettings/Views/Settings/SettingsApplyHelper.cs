// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
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

        var changeLines = pendingChanges
            .Select(change => $"- {GetSettingName(change.ConfigEntry)}: {FormatSettingValue(change.ConfigEntry, change.PendingValue)}")
            .ToList();

        var contentText = string.Join(Environment.NewLine, changeLines);

        var runningDistros = WslCoreConfigInterface.GetRunningDistributionNames();

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

        if (runningDistros.Count > 0)
        {
            contentPanel.Children.Add(new TextBlock
            {
                Text = "Settings_ApplyChangesDialogRunningDistros".GetLocalized(),
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 8, 0, 0),
            });
            contentPanel.Children.Add(new TextBlock
            {
                Text = string.Join(", ", runningDistros),
                TextWrapping = TextWrapping.Wrap,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            });
            contentPanel.Children.Add(new TextBlock
            {
                Text = "Settings_ApplyChangesDialogWillRestart".GetLocalized(),
                TextWrapping = TextWrapping.Wrap,
                FontStyle = Windows.UI.Text.FontStyle.Italic,
                Margin = new Thickness(0, 4, 0, 0),
            });
        }
        else
        {
            contentPanel.Children.Add(new TextBlock
            {
                Text = "Settings_ApplyChangesDialogNoRunningDistros".GetLocalized(),
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 8, 0, 0),
            });
        }

        var dialog = new ContentDialog
        {
            XamlRoot = xamlRoot,
            Title = "Settings_ApplyChangesDialogTitle".GetLocalized(),
            Content = contentPanel,
            PrimaryButtonText = "Settings_ApplyChangesDialogRestartButton".GetLocalized(),
            CloseButtonText = "Settings_ApplyChangesDialogCancelButton".GetLocalized(),
            DefaultButton = ContentDialogButton.Primary,
        };

        var result = await dialog.ShowAsync();
        if (result != ContentDialogResult.Primary)
        {
            return;
        }

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
            });

            wslConfigService.ClearPendingChanges();

            // Relaunch previously-running distributions
            foreach (var name in runningDistros)
            {
                try
                {
                    Process.Start(new ProcessStartInfo
                    {
                        FileName = wslPath,
                        Arguments = $"-d {name}",
                        UseShellExecute = true,
                    });
                }
                catch
                {
                    // Best-effort — don't fail the whole operation if one distro can't relaunch
                }
            }
        }
        catch (Exception ex)
        {
            await ShowFailureDialogAsync(xamlRoot, string.Format("Settings_ApplyChangesDialogRestartFailed".GetLocalized(), ex.Message));
        }
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

    private static string FormatSettingValue(WslConfigEntry entry, object value)
    {
        if (value is ulong ulongValue)
        {
            if (entry == WslConfigEntry.MemorySizeBytes || entry == WslConfigEntry.SwapSizeBytes || entry == WslConfigEntry.VhdSizeBytes)
            {
                return $"{ulongValue / Constants.MB} MB";
            }

            return ulongValue.ToString();
        }

        if (value is bool boolValue)
        {
            return boolValue ? "true" : "false";
        }

        if (value is int intValue)
        {
            if (entry == WslConfigEntry.InitialAutoProxyTimeout || entry == WslConfigEntry.VMIdleTimeout)
            {
                return $"{intValue} ms";
            }

            return intValue.ToString();
        }

        return value?.ToString() ?? string.Empty;
    }
}
