// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.UI.Xaml.Controls;
using WslSettings.Contracts.Services;
using WslSettings.ViewModels.OOBE;
using WslSettings.ViewModels.Settings;
using WslSettings.Views.OOBE;
using WslSettings.Views.Settings;

namespace WslSettings.Services;

public class PageService : IPageService
{
    private readonly Dictionary<string, Type> _pages = new();

    public PageService()
    {
        Configure<MemAndProcViewModel, MemAndProcPage>();
        Configure<FileSystemViewModel, FileSystemPage>();
        Configure<NetworkingViewModel, NetworkingPage>();
        Configure<OptionalFeaturesViewModel, OptionalFeaturesPage>();
        Configure<DeveloperViewModel, DeveloperPage>();
        Configure<AboutViewModel, AboutPage>();

        Configure<GeneralViewModel, GeneralPage>();
        Configure<WorkingAcrossFileSystemsViewModel, WorkingAcrossFileSystemsPage>();
        Configure<VSCodeIntegrationViewModel, VSCodeIntegrationPage>();
        Configure<GUIAppsViewModel, GUIAppsPage>();
        Configure<GPUAccelerationViewModel, GPUAccelerationPage>();
        Configure<DockerDesktopIntegrationViewModel, DockerDesktopIntegrationPage>();
        Configure<NetworkingIntegrationViewModel, NetworkingIntegrationPage>();
        Configure<DistroManagementViewModel, DistroManagementPage>();
    }

    public Type GetPageType(string key)
    {
        Type? pageType;
        lock (_pages)
        {
            if (!_pages.TryGetValue(key, out pageType))
            {
                throw new ArgumentException($"Page not found: {key}. Did you forget to call PageService.Configure?");
            }
        }

        return pageType;
    }

    private void Configure<VM, V>()
        where VM : ObservableObject
        where V : Page
    {
        lock (_pages)
        {
            var key = typeof(VM).FullName!;
            if (_pages.ContainsKey(key))
            {
                throw new ArgumentException($"The key {key} is already configured in PageService");
            }

            var type = typeof(V);
            if (_pages.ContainsValue(type))
            {
                throw new ArgumentException($"This type is already configured with key {_pages.First(p => p.Value == type).Key}");
            }

            _pages.Add(key, type);
        }
    }
}
