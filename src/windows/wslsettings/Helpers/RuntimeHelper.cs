// Copyright (C) Microsoft Corporation. All rights reserved.

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
}