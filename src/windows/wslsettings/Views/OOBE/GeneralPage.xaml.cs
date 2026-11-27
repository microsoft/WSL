// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class GeneralPage : Page
{
    public GeneralViewModel ViewModel
    {
        get;
    }

    public GeneralPage()
    {
        ViewModel = App.GetService<GeneralViewModel>();
        InitializeComponent();

        // The OOBE window can close without navigating away from this page, so also unsubscribe when unloaded.
        Unloaded += (_, _) => ViewModel.Unsubscribe();
    }

    override protected void OnNavigatedFrom(NavigationEventArgs e)
    {
        ViewModel.Unsubscribe();
    }
}