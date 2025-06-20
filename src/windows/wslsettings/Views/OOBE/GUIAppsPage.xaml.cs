// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class GUIAppsPage : Page
{
    public GUIAppsViewModel ViewModel
    {
        get;
    }

    public GUIAppsPage()
    {
        ViewModel = App.GetService<GUIAppsViewModel>();
        InitializeComponent();
    }
}
