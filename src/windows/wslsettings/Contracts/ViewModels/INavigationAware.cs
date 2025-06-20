// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Contracts.ViewModels;

public interface INavigationAware
{
    void OnNavigatedTo(object parameter);

    void OnNavigatedFrom();
}
