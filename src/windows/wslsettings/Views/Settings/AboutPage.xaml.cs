// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.Settings;

namespace WslSettings.Views.Settings;

public sealed partial class AboutPage : Page
{
    public AboutViewModel ViewModel
    {
        get;
    }

    public AboutPage()
    {
        ViewModel = App.GetService<AboutViewModel>();
        InitializeComponent();
    }
}
