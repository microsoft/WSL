// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.Windows.ApplicationModel.Resources;

namespace WslSettings.Helpers;

public static class ResourceExtensions
{
    private static readonly ResourceLoader _resourceLoader = new();

    public static string GetLocalized(this string resourceKey) => _resourceLoader.GetString(resourceKey);
}
