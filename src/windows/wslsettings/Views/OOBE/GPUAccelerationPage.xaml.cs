// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml.Controls;
using WslSettings.ViewModels.OOBE;
namespace WslSettings.Views.OOBE;

public sealed partial class GPUAccelerationPage : Page
{
    public GPUAccelerationViewModel ViewModel
    {
        get;
    }

    public GPUAccelerationPage()
    {
        ViewModel = App.GetService<GPUAccelerationViewModel>();
        InitializeComponent();
    }
}
