// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Contracts.Services;

public interface IWindowService
{
    public enum WindowId
    {
        MainWindow,
        OOBEWindow
    }

    WindowEx CreateOrGetWindow(WindowId windowId);
}