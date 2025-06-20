// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class WorkingAcrossFileSystemsPage : Page
{
    public WorkingAcrossFileSystemsViewModel ViewModel
    {
        get;
    }

    public WorkingAcrossFileSystemsPage()
    {
        ViewModel = App.GetService<WorkingAcrossFileSystemsViewModel>();
        InitializeComponent();
    }
}
