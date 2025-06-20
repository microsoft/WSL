// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using System.Reflection;

namespace WslSettings.ViewModels.Settings;

public partial class AboutViewModel : ObservableRecipient
{
    public AboutViewModel()
    {
    }

    public string VersionDescription
    {
        get
        {
            Version version = Assembly.GetExecutingAssembly().GetName().Version!;
            return $"{version.Major}.{version.Minor}.{version.Build}";
        }
    }
}