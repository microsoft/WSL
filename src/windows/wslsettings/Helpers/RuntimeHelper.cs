// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.WinUI.Controls;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using Windows.ApplicationModel;
using Windows.Storage.Pickers;

namespace WslSettings.Helpers;

public class RuntimeHelper
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetCurrentPackageFullName(ref int packageFullNameLength, StringBuilder? packageFullName);

    public static Windows.Foundation.IAsyncOperation<Windows.Storage.StorageFile> PickSingleFileAsync(List<string>? fileTypeFilters = null)
    {
        // Create a file picker
        var openPicker = new FileOpenPicker();
        var hWnd = WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow);

        // Initialize the file picker with the window handle (HWND).
        WinRT.Interop.InitializeWithWindow.Initialize(openPicker, hWnd);

        fileTypeFilters ??= ["*"];

        // Set options for your file picker
        openPicker.ViewMode = PickerViewMode.List;
        foreach (string fileTypeFilter in fileTypeFilters)
        {
            openPicker.FileTypeFilter.Add(fileTypeFilter);
        }

        // Open the picker for the user to pick a file
        return openPicker.PickSingleFileAsync();
    }

    public static void TryMoveFocusPreviousControl(Button? button)
    {
        if (button == null)
        {
            return;
        }

        FindNextElementOptions fneo = new() { SearchRoot = button.XamlRoot.Content };
        FocusManager.TryMoveFocus(FocusNavigationDirection.Previous, fneo);
    }

    public static void SetupSettingsExpanderFocusManagement(Microsoft.UI.Xaml.FrameworkElement expander, Microsoft.UI.Xaml.Controls.Control firstFocusableElement)
    {
        if (expander is CommunityToolkit.WinUI.Controls.SettingsExpander settingsExpander)
        {
            settingsExpander.RegisterPropertyChangedCallback(CommunityToolkit.WinUI.Controls.SettingsExpander.IsExpandedProperty, (sender, dp) =>
            {
                if (sender is CommunityToolkit.WinUI.Controls.SettingsExpander se && se.IsExpanded)
                {
                    System.EventHandler<object>? layoutHandler = null;
                    layoutHandler = (s, e) =>
                    {
                        se.LayoutUpdated -= layoutHandler;
                        firstFocusableElement.Focus(Microsoft.UI.Xaml.FocusState.Keyboard);
                    };

                    se.LayoutUpdated += layoutHandler;
                }
            });
        }
    }

    public static void SetupExpanderFocusManagementByName(Microsoft.UI.Xaml.FrameworkElement parent, string expanderName, string textBoxName)
    {
        var expander = parent.FindName(expanderName) as Microsoft.UI.Xaml.FrameworkElement;
        var textBox = parent.FindName(textBoxName) as Microsoft.UI.Xaml.Controls.Control;

        if (expander != null && textBox != null)
        {
            SetupSettingsExpanderFocusManagement(expander, textBox);
        }
    }
}