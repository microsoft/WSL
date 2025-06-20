// Copyright (c) Microsoft Corporation

using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using System.Runtime.InteropServices;
using Windows.Graphics;
using WinUIEx.Messaging;
using Windows.UI.ViewManagement;
using Windows.UI.WindowManagement;
using System.Runtime.Intrinsics.Arm;

namespace WslSettings;

/// <summary>
/// An empty window that can be used on its own or navigated to within a Frame.
/// </summary>
public sealed partial class OOBEWindow : WindowEx, IDisposable
{
    [DllImport("User32.dll")]
    internal static extern int GetDpiForWindow(IntPtr hwnd);

    private const int ExpectedWidth = 1100;
    private const int ExpectedHeight = 700;
    private const int DefaultDPI = 96;
    private int currentDPI;
    private IntPtr hWnd;
    private Microsoft.UI.Dispatching.DispatcherQueue dispatcherQueue = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
    private UISettings settings = new UISettings();
    private WindowMessageMonitor msgMonitor;
    private bool disposedValue;

    public OOBEWindow()
    {
        InitializeComponent();

        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets/wsl.ico"));
        Content = null;
        Title = "Settings_OOBEDisplayName".GetLocalized();

        // Theme change code picked from https://github.com/microsoft/WinUI-Gallery/pull/1239
        settings.ColorValuesChanged += Settings_ColorValuesChanged; // cannot use FrameworkElement.ActualThemeChanged event

        WindowManager.Get(this).IsMinimizable = false;
        WindowManager.Get(this).IsMaximizable = false;

        hWnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        currentDPI = GetDpiForWindow(hWnd);
        ResizeWindow();

        SizeChanged += Window_SizeChanged;

        msgMonitor = new WindowMessageMonitor(this);
        msgMonitor.WindowMessageReceived += (_, e) =>
        {
            const int WM_NCLBUTTONDBLCLK = 0x00A3;
            if (e.Message.MessageId == WM_NCLBUTTONDBLCLK)
            {
                // Disable double click on title bar to maximize window
                e.Result = 0;
                e.Handled = true;
            }
        };
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

    private void Window_SizeChanged(object sender, WindowSizeChangedEventArgs args)
    {
        var dpi = GetDpiForWindow(hWnd);
        if (currentDPI != dpi)
        {
            // Reacting to a DPI change. Should not cause a resize -> sizeChanged loop.
            currentDPI = dpi;
            ResizeWindow();
        }
    }

    private void Window_Closed(object sender, WindowEventArgs args)
    {
        App.OOBEWindow = null;
        App.MainWindow?.CloseHiddenWindow();
        Dispose();
    }

    private void ResizeWindow()
    {
        float scalingFactor = (float)currentDPI / DefaultDPI;
        int width = (int)(ExpectedWidth * scalingFactor);
        int height = (int)(ExpectedHeight * scalingFactor);
        SizeInt32 size;
        size.Width = width;
        size.Height = height;
        AppWindow.Resize(size);
    }

    private void Dispose(bool disposing)
    {
        if (!disposedValue)
        {
            msgMonitor?.Dispose();
            settings.ColorValuesChanged -= Settings_ColorValuesChanged;
            disposedValue = true;
        }
    }

    public void Dispose()
    {
        // Do not change this code. Put cleanup code in 'Dispose(bool disposing)' method
        Dispose(disposing: true);
        GC.SuppressFinalize(this);
    }
}