// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class VSCodeIntegrationPage : Page
{
    public VSCodeIntegrationViewModel ViewModel
    {
        get;
    }

    public VSCodeIntegrationPage()
    {
        ViewModel = App.GetService<VSCodeIntegrationViewModel>();
        InitializeComponent();
    }
}
