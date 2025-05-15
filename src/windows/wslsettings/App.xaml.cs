// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.UI.Xaml;
using WslSettings.Activation;
using WslSettings.Contracts.Services;
using WslSettings.Services;
using WslSettings.ViewModels;
using WslSettings.ViewModels.OOBE;
using WslSettings.ViewModels.Settings;
using WslSettings.Views.OOBE;
using WslSettings.Views.Settings;

namespace WslSettings;

// To learn more about WinUI 3, see https://docs.microsoft.com/windows/apps/winui/winui3/.
public partial class App : Application
{
    // The .NET Generic Host provides dependency injection, configuration, logging, and other services.
    // https://docs.microsoft.com/dotnet/core/extensions/generic-host
    // https://docs.microsoft.com/dotnet/core/extensions/dependency-injection
    // https://docs.microsoft.com/dotnet/core/extensions/configuration
    // https://docs.microsoft.com/dotnet/core/extensions/logging
    public IHost Host
    {
        get;
    }

    public static T GetService<T>()
        where T : class
    {
        if ((App.Current as App)!.Host.Services.GetService(typeof(T)) is not T service)
        {
            throw new ArgumentException($"{typeof(T)} needs to be registered in ConfigureServices within App.xaml.cs.");
        }

        return service;
    }

    public static MainWindow? MainWindow { get; set; }

    public static OOBEWindow? OOBEWindow { get; set; }

    public static UIElement? AppTitlebar { get; set; }

    public App()
    {
        InitializeComponent();

        Host = Microsoft.Extensions.Hosting.Host.
        CreateDefaultBuilder().
        UseContentRoot(AppContext.BaseDirectory).
        ConfigureServices((context, services) =>
        {
            // Default Activation Handler
            services.AddTransient<ActivationHandler<LaunchActivatedEventArgs>, DefaultActivationHandler>();

            // Other Activation Handlers
            services.AddTransient<IActivationHandler, ProtocolActivationHandler>();

            // Services
            services.AddTransient<INavigationViewService, NavigationViewService>();

            services.AddSingleton<IActivationService, ActivationService>();
            services.AddSingleton<IPageService, PageService>();
            services.AddSingleton<INavigationService, NavigationService>();
            services.AddSingleton<IWindowService, WindowService>();
            services.AddSingleton<IWslConfigService, WslConfigService>();

            // Views and ViewModels
            services.AddTransient<AboutViewModel>();
            services.AddTransient<AboutPage>();
            services.AddTransient<DeveloperViewModel>();
            services.AddTransient<DeveloperPage>();
            services.AddTransient<FileSystemViewModel>();
            services.AddTransient<FileSystemPage>();
            services.AddTransient<MemAndProcViewModel>();
            services.AddTransient<MemAndProcPage>();
            services.AddTransient<NetworkingViewModel>();
            services.AddTransient<NetworkingPage>();
            services.AddTransient<OptionalFeaturesViewModel>();
            services.AddTransient<OptionalFeaturesPage>();
            services.AddTransient<ShellViewModel>();
            services.AddTransient<Views.Settings.ShellPage>();

            services.AddTransient<DistroManagementViewModel>();
            services.AddTransient<DistroManagementPage>();
            services.AddTransient<DockerDesktopIntegrationViewModel>();
            services.AddTransient<DockerDesktopIntegrationPage>();
            services.AddTransient<GeneralViewModel>();
            services.AddTransient<GeneralPage>();
            services.AddTransient<GPUAccelerationViewModel>();
            services.AddTransient<GPUAccelerationPage>();
            services.AddTransient<GUIAppsViewModel>();
            services.AddTransient<GUIAppsPage>();
            services.AddTransient<NetworkingIntegrationViewModel>();
            services.AddTransient<NetworkingIntegrationPage>();
            services.AddTransient<VSCodeIntegrationViewModel>();
            services.AddTransient<VSCodeIntegrationPage>();
            services.AddTransient<WorkingAcrossFileSystemsViewModel>();
            services.AddTransient<WorkingAcrossFileSystemsPage>();
            services.AddTransient<Views.OOBE.ShellPage>();

            // Configuration
        }).
        Build();

        UnhandledException += App_UnhandledException;
    }

    private void App_UnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
    {
        // TODO: Log and handle exceptions as appropriate.
        // https://docs.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.application.unhandledexception.
    }

    protected async override void OnLaunched(LaunchActivatedEventArgs args)
    {
        base.OnLaunched(args);

        await App.GetService<IActivationService>().ActivateAsync(args);
    }
}
