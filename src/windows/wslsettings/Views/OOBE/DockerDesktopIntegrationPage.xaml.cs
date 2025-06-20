// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class DockerDesktopIntegrationPage : Page
{
    public DockerDesktopIntegrationViewModel ViewModel
    {
        get;
    }

    public DockerDesktopIntegrationPage()
    {
        ViewModel = App.GetService<DockerDesktopIntegrationViewModel>();
        InitializeComponent();
    }
}
