// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class DistroManagementPage : Page
{
    public DistroManagementViewModel ViewModel
    {
        get;
    }

    public DistroManagementPage()
    {
        ViewModel = App.GetService<DistroManagementViewModel>();
        InitializeComponent();
    }
}
