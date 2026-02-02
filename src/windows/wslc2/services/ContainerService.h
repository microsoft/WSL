// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"

namespace wsl::windows::wslc::services
{
    struct ContainerService
    {
        void StartContainer(std::wstring containerName);
    };
}