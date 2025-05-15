// Copyright (C) Microsoft Corporation. All rights reserved.

using WslSettings.Contracts.Services;

namespace WslSettings.Services;

public class WindowService : IWindowService
{
    public WindowService()
    {
    }

    public WindowEx CreateOrGetWindow(IWindowService.WindowId windowId)
    {
        WindowEx window;
        switch (windowId)
        {
            case IWindowService.WindowId.MainWindow:
                if (App.MainWindow == null)
                {
                    App.MainWindow = new MainWindow();
                }

                if (App.MainWindow!.Content == null)
                {
                    App.MainWindow.Content = App.GetService<Views.Settings.ShellPage>();
                }

                window = App.MainWindow;
                break;
            case IWindowService.WindowId.OOBEWindow:
                if (App.OOBEWindow == null)
                {
                    App.OOBEWindow = new OOBEWindow();
                }

                if (App.OOBEWindow.Content == null)
                {
                    App.OOBEWindow.Content = App.GetService<Views.OOBE.ShellPage>();
                }

                window = App.OOBEWindow;
                break;
            default:
                throw new ArgumentException("Invalid ActivationWindowId");
        }

        return window;
    }
}