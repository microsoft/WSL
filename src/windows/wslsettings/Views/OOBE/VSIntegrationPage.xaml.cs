// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class VSIntegrationPage : Page
{
    public VSIntegrationViewModel ViewModel
    {
        get;
    }

    public VSIntegrationPage()
    {
        ViewModel = App.GetService<VSIntegrationViewModel>();
        InitializeComponent();
    }
}
