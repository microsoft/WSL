// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Activation;

public interface IActivationHandler
{
    bool CanHandle(object args);

    Task HandleAsync(object args);
}
