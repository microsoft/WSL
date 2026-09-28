// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
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
    }
}
