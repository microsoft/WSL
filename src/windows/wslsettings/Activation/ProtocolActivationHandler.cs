// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;
using System.Runtime.CompilerServices;
using WslSettings.Contracts.Services;
using WslSettings.ViewModels.OOBE;
using WslSettings.ViewModels.Settings;

namespace WslSettings.Activation;

public class ProtocolActivationHandler : ActivationHandler<LaunchActivatedEventArgs>
{
    public ProtocolActivationHandler(INavigationService navigationService) : base(navigationService)
    {
    }

    protected override bool CanHandleInternal(LaunchActivatedEventArgs args)
    {
        // None of the ActivationHandlers has handled the activation.
        return AppInstance.GetCurrent().GetActivatedEventArgs().Kind == ExtendedActivationKind.Protocol &&
            _navigationService.Frame?.Content == null;
    }

    private static IWindowService.WindowId ResolveWindowId(Uri uri)
    {
        IWindowService.WindowId windowId = IWindowService.WindowId.MainWindow;
        switch (uri.Host.ToLower())
        {
            case "settings":
                windowId = IWindowService.WindowId.MainWindow;
                break;
            case "oobe":
                windowId = IWindowService.WindowId.OOBEWindow;
                break;
            default:
                windowId = IWindowService.WindowId.MainWindow;
                break;
        }

        return windowId;
    }

    private static string ResolvePageKey(Uri uri, IWindowService.WindowId windowId)
    {
        string pageName = uri.LocalPath.ToLower().Trim('/');
        string pageKey = typeof(MemAndProcViewModel).FullName!;
        switch (windowId)
        {
            case IWindowService.WindowId.MainWindow:
                switch (pageName)
                {
                    case "memandproc":
                        pageKey = typeof(MemAndProcViewModel).FullName!;
                        break;
                    case "filesystem":
                        pageKey = typeof(FileSystemViewModel).FullName!;
                        break;
                    case "networking":
                        pageKey = typeof(NetworkingViewModel).FullName!;
                        break;
                    case "optfeatures":
                        pageKey = typeof(OptionalFeaturesViewModel).FullName!;
                        break;
                    case "developer":
                        pageKey = typeof(DeveloperViewModel).FullName!;
                        break;
                    case "about":
                        pageKey = typeof(AboutViewModel).FullName!;
                        break;
                    default:
                        pageKey = typeof(MemAndProcViewModel).FullName!;
                        break;
                }
                break;
            case IWindowService.WindowId.OOBEWindow:
                switch (pageName)
                {
                    case "general":
                        pageKey = typeof(GeneralViewModel).FullName!;
                        break;
                    case "crossosfiles":
                        pageKey = typeof(WorkingAcrossFileSystemsViewModel).FullName!;
                        break;
                    case "guiapps":
                        pageKey = typeof(GUIAppsViewModel).FullName!;
                        break;
                    case "vscodeint":
                        pageKey = typeof(VSCodeIntegrationViewModel).FullName!;
                        break;
                    case "gpuaccel":
                        pageKey = typeof(GPUAccelerationViewModel).FullName!;
                        break;
                    case "dockerint":
                        pageKey = typeof(DockerDesktopIntegrationViewModel).FullName!;
                        break;
                    case "networkingint":
                        pageKey = typeof(NetworkingIntegrationViewModel).FullName!;
                        break;
                    case "distromgmt":
                        pageKey = typeof(DistroManagementViewModel).FullName!;
                        break;
                    default:
                        pageKey = typeof(GeneralViewModel).FullName!;
                        break;
                }
                break;
            default:
                pageKey = typeof(MemAndProcViewModel).FullName!;
                break;
        }

        return pageKey;
    }

    protected async override Task HandleInternalAsync(LaunchActivatedEventArgs args)
    {
        Windows.ApplicationModel.Activation.IProtocolActivatedEventArgs eventArgs =
            (Windows.ApplicationModel.Activation.IProtocolActivatedEventArgs)AppInstance.GetCurrent().GetActivatedEventArgs().Data;
        Uri uri = eventArgs.Uri;
        IWindowService.WindowId windowId = ResolveWindowId(uri);

        Navigate(windowId, ResolvePageKey(uri, windowId), args);

        await Task.CompletedTask;
    }
}
