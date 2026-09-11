// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class WSLContainerPage : Page
{
    public WSLContainerViewModel ViewModel
    {
        get;
    }

    public WSLContainerPage()
    {
        ViewModel = App.GetService<WSLContainerViewModel>();
        InitializeComponent();
    }
}
