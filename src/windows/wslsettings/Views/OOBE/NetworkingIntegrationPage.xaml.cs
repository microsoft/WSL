// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class NetworkingIntegrationPage : Page
{
    public NetworkingIntegrationViewModel ViewModel
    {
        get;
    }

    public NetworkingIntegrationPage()
    {
        ViewModel = App.GetService<NetworkingIntegrationViewModel>();
        InitializeComponent();
    }
}
