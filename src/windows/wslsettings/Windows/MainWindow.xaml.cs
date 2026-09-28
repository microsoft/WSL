// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Windows.UI.ViewManagement;

namespace WslSettings;

public sealed partial class MainWindow : WindowEx
{
    private Microsoft.UI.Dispatching.DispatcherQueue dispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();

    private UISettings settings = new UISettings();

    public MainWindow()
    {
        InitializeComponent();

        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets/wsl.ico"));
        Content = null;
        Title = "Settings_AppDisplayName".GetLocalized();

        // Theme change code picked from https://github.com/microsoft/WinUI-Gallery/pull/1239
        settings.ColorValuesChanged += Settings_ColorValuesChanged; // cannot use FrameworkElement.ActualThemeChanged event
    }

    // this handles updating the caption button colors correctly when windows system theme is changed
    // while the app is open
    private void Settings_ColorValuesChanged(UISettings sender, object args)
    {
        // This calls comes off-thread, hence we will need to dispatch it to current app's thread
        dispatcherQueue.TryEnqueue(() =>
        {
            TitleBarHelper.ApplySystemThemeToCaptionButtons(this);
        });
    }

    public void CloseHiddenWindow()
    {
        if (!Visible)
        {
            Close();
        }
    }

    private void Window_Closed(object sender, WindowEventArgs args)
    {
        if (App.OOBEWindow == null)
        {
            App.MainWindow = null;
            settings.ColorValuesChanged -= Settings_ColorValuesChanged;
        }
        else
        {
            args.Handled = true;
            this.Hide();
        }
    }
}
