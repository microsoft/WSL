/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InstallProgress.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK InstallProgress class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.InstallProgress.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct InstallProgress : InstallProgressT<InstallProgress>
{
    InstallProgress() = default;
    InstallProgress(winrt::Microsoft::WSL::Containers::ComponentFlags component, uint32_t progress, uint32_t total);

    winrt::Microsoft::WSL::Containers::ComponentFlags Component();
    uint32_t Progress();
    uint32_t Total();

private:
    winrt::Microsoft::WSL::Containers::ComponentFlags m_component{};
    uint32_t m_progress{};
    uint32_t m_total{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(InstallProgress);
