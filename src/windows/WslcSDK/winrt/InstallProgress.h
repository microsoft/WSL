/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InstallProgress.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK InstallProgress class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.InstallProgress.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct InstallProgress : InstallProgressT<InstallProgress>
{
    InstallProgress() = default;

    winrt::Microsoft::WSL::Containers::ComponentFlags Component();
    uint32_t Progress();
    uint32_t Total();
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
