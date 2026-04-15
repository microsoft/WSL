// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using WslSettings.Contracts.Services;
using WslSettings.ViewModels.Settings;

namespace WslSettings.Views.Settings;

public sealed partial class DeveloperPage : Page
{
    public DeveloperViewModel ViewModel
    {
        get;
    }

    public DeveloperPage()
    {
        ViewModel = App.GetService<DeveloperViewModel>();
        InitializeComponent();
        Settings_ErrorTryAgainLater.RegisterPropertyChangedCallback(TextBlock.VisibilityProperty, (s, e) =>
        {
            if (ViewModel.ErrorVisibility)
            {
                FrameworkElementAutomationPeer.FromElement(Settings_ErrorTryAgainLater).RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
            }
        });

        this.Loaded += OnPageLoaded;
    }

    private void OnPageLoaded(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        DeveloperPageRoot.Focus(FocusState.Programmatic);
        RuntimeHelper.SetupExpanderFocusManagementByName(this, "CustomKernelPathExpander", "CustomKernelPathTextBox");
        RuntimeHelper.SetupExpanderFocusManagementByName(this, "CustomKernelModulesPathExpander", "CustomKernelModulesPathTextBox");
        RuntimeHelper.SetupExpanderFocusManagementByName(this, "CustomSystemDistroPathExpander", "CustomSystemDistroPathTextBox");
    }

    override protected void OnNavigatedFrom(NavigationEventArgs e)
    {
        App.GetService<IWslConfigService>().WslConfigChanged -= ViewModel.OnConfigChanged;
    }

    public async void CustomKernelPath_Click(object sender, RoutedEventArgs e)
    {
        // Open the picker for the user to pick a file
        Windows.Storage.StorageFile file = await RuntimeHelper.PickSingleFileAsync();
        if (file != null)
        {
            ViewModel.CustomKernelPath = file.Path;
        }
    }

    public async void CustomKernelModulesPath_Click(object sender, RoutedEventArgs e)
    {
        // Open the picker for the user to pick a file
        Windows.Storage.StorageFile file = await RuntimeHelper.PickSingleFileAsync([".vhd", ".vhdx"]);
        if (file != null)
        {
            ViewModel.CustomKernelModulesPath = file.Path;
        }
    }

    public async void CustomSystemDistroPath_Click(object sender, RoutedEventArgs e)
    {
        // Open the picker for the user to pick a file
        Windows.Storage.StorageFile file = await RuntimeHelper.PickSingleFileAsync([".img", ".vhd"]);
        if (file != null)
        {
            ViewModel.CustomSystemDistroPath = file.Path;
        }
    }
}
