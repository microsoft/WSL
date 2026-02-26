// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using WslSettings.Contracts.Services;
using WslSettings.ViewModels.Settings;

namespace WslSettings.Views.Settings;

public sealed partial class MemAndProcPage : Page
{
    public MemAndProcViewModel ViewModel
    {
        get;
    }

    public MemAndProcPage()
    {
        ViewModel = App.GetService<MemAndProcViewModel>();
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
        MemAndProcPageRoot.Focus(FocusState.Programmatic);
        RuntimeHelper.SetupExpanderFocusManagementByName(this, "ProcCountExpander", "ProcCountTextBox");
        RuntimeHelper.SetupExpanderFocusManagementByName(this, "MemorySizeExpander", "MemorySizeTextBox");
        RuntimeHelper.SetupExpanderFocusManagementByName(this, "SwapSizeExpander", "SwapSizeTextBox");
        RuntimeHelper.SetupExpanderFocusManagementByName(this, "SwapFilePathExpander", "SwapFilePathTextBox");
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

    public async void SwapFilePath_Click(object sender, RoutedEventArgs e)
    {
        // Open the picker for the user to pick a file
        Windows.Storage.StorageFile file = await RuntimeHelper.PickSingleFileAsync([".vhdx"]);
        if (file != null)
        {
            ViewModel.SwapFilePath = file.Path;
        }
    }

    private void ProcCountTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (sender == null)
        {
            return;
        }

        TextBox? textBox = sender as TextBox;
        ViewModel.SetProcCount_ResetEnabled(textBox!.Text);
    }

    private void MemorySizeTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (sender == null)
        {
            return;
        }

        TextBox? textBox = sender as TextBox;
        ViewModel.SetMemorySize_ResetEnabled(textBox!.Text);
    }

    private void SwapSizeTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (sender == null)
        {
            return;
        }

        TextBox? textBox = sender as TextBox;
        ViewModel.SetSwapSize_ResetEnabled(textBox!.Text);
    }
}
