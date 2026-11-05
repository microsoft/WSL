// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;

namespace WslSettings.Helpers;

public static class FrameExtensions
{
    public static object? GetPageViewModel(this Frame frame) => frame?.Content switch
    {
        Views.Settings.MemAndProcPage page => page.ViewModel,
        Views.Settings.FileSystemPage page => page.ViewModel,
        Views.Settings.NetworkingPage page => page.ViewModel,
        Views.Settings.OptionalFeaturesPage page => page.ViewModel,
        Views.Settings.DeveloperPage page => page.ViewModel,
        Views.Settings.AboutPage page => page.ViewModel,
        Views.Settings.ShellPage page => page.ViewModel,
        Views.OOBE.GeneralPage page => page.ViewModel,
        Views.OOBE.WSLContainerPage page => page.ViewModel,
        Views.OOBE.WorkingAcrossFileSystemsPage page => page.ViewModel,
        Views.OOBE.VSCodeIntegrationPage page => page.ViewModel,
        Views.OOBE.VSIntegrationPage page => page.ViewModel,
        Views.OOBE.GUIAppsPage page => page.ViewModel,
        Views.OOBE.GPUAccelerationPage page => page.ViewModel,
        Views.OOBE.DockerDesktopIntegrationPage page => page.ViewModel,
        Views.OOBE.NetworkingIntegrationPage page => page.ViewModel,
        Views.OOBE.DistroManagementPage page => page.ViewModel,
        Views.OOBE.ShellPage page => page.ViewModel,
        _ => null
    };
}
