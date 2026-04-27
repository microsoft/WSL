// Copyright (C) Microsoft Corporation. All rights reserved.

using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using WslSettings.Contracts.Services;

namespace WslSettings.Views.Settings;

internal static class SettingsApplyHelper
{
    public static async Task ShowApplyChangesDialogAsync(XamlRoot xamlRoot)
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
            changeLines.Add($"- {GetSettingDisplayName(change.ConfigEntry)}: {FormatValue(change.ConfigEntry, change.PendingValue)}");
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
            CloseButtonText = "Settings_ApplyChangesDialogCloseButton".GetLocalized(),
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

    private static async Task ShowFailureDialogAsync(XamlRoot xamlRoot, string message)
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

    private static string GetSettingDisplayName(WslConfigEntry entry)
    {
        // Use existing Settings page resource keys so dialog matches page terminology
        if (SettingDisplayNameResources.TryGetValue(entry, out var resourceKey))
        {
            var localized = resourceKey.GetLocalized();
            if (!string.IsNullOrEmpty(localized) && localized != resourceKey)
            {
                return localized;
            }
        }

        // Fallback: type name (keeps something useful even if resx missing)
        return entry.ToString();
    }

    private static readonly IReadOnlyDictionary<WslConfigEntry, string> SettingDisplayNameResources =
        new Dictionary<WslConfigEntry, string>
        {
            // ResourceLoader.GetString() requires '/' (not '.') as the separator for x:Uid property resources
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

    private static string FormatValue(WslConfigEntry entry, object value)
    {
        switch (entry.GetValueKind())
        {
            case WslConfigValueKind.UInt64:
                return string.Format("Settings_MegabyteStringFormat".GetLocalized(), (ulong)value / Constants.MB);
            case WslConfigValueKind.Int32:
                switch (entry)
                {
                    case WslConfigEntry.InitialAutoProxyTimeout:
                    case WslConfigEntry.VMIdleTimeout:
                        return string.Format("Settings_MillisecondsStringFormat".GetLocalized(), (int)value);
                    default:
                        return ((int)value).ToString();
                }
            case WslConfigValueKind.String:
                return (string?)value ?? string.Empty;
            case WslConfigValueKind.NetworkingConfiguration:
                return FormatEnum((NetworkingConfiguration)value);
            case WslConfigValueKind.MemoryReclaimMode:
                return FormatEnum((MemoryReclaimMode)value);
            default:
                return FormatBool((bool)value);
        }
    }

    private static string FormatBool(bool value)
    {
        var localized = value
            ? "Settings_BooleanTrueText".GetLocalized()
            : "Settings_BooleanFalseText".GetLocalized();
        return string.IsNullOrEmpty(localized)
            ? (value ? bool.TrueString : bool.FalseString)
            : localized;
    }

    private static string FormatEnum<TEnum>(TEnum value) where TEnum : struct, Enum
    {
        // Try resource lookup first using the pattern "Settings_{EnumTypeName}_{EnumValue}".
        // GetLocalized throws COMException when the key doesn't exist, so fall back to the raw name.
        try
        {
            var resourceKey = $"Settings_{typeof(TEnum).Name}_{value}";
            var localized = resourceKey.GetLocalized();
            if (!string.IsNullOrEmpty(localized) && localized != resourceKey)
            {
                return localized;
            }
        }
        catch (COMException)
        {
        }

        return value.ToString();
    }
}
