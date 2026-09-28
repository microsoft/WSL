// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Contracts.Services;

public interface IActivationService
{
    Task ActivateAsync(object activationArgs);
}
