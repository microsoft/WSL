﻿// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using WslSettings.Contracts.Services;
using WslSettings.ViewModels.Settings;

namespace WslSettings.Views.Settings;

public sealed partial class FileSystemPage : Page
{
    public FileSystemViewModel ViewModel
    {
        get;
    }

    public FileSystemPage()
    {
        ViewModel = App.GetService<FileSystemViewModel>();
        InitializeComponent();
        Settings_ErrorTryAgainLater.RegisterPropertyChangedCallback(TextBlock.VisibilityProperty, (s, e) =>
        {
            if (ViewModel.ErrorVisibility)
            {
                FrameworkElementAutomationPeer.FromElement(Settings_ErrorTryAgainLater).RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
            }
        });
    }

    override protected void OnNavigatedFrom(NavigationEventArgs e)
    {
        App.GetService<IWslConfigService>().WslConfigChanged -= ViewModel.OnConfigChanged;
    }

    private void Settings_ResetButton_Click(object sender, Microsoft.UI.Xaml.RoutedEventArgs e)
    {
        if (sender == null)
        {
            return;
        }

        RuntimeHelper.TryMoveFocusPreviousControl(sender as Button);
    }

    private void DefaultVHDSizeTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (sender == null)
        {
            return;
        }

        TextBox? textBox = sender as TextBox;
        ViewModel.SetDefaultVHDSize_ResetEnabled(textBox!.Text);
    }
}
