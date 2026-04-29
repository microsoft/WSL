/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InstallProgress.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK InstallProgress class.

--*/

#include "precomp.h"
#include "InstallProgress.h"
#include "Microsoft.WSL.Containers.InstallProgress.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
InstallProgress::InstallProgress(ComponentFlags component, uint32_t progress, uint32_t total) :
    m_component(component), m_progress(progress), m_total(total)
{
}

ComponentFlags InstallProgress::Component()
{
    return m_component;
}

uint32_t InstallProgress::Progress()
{
    return m_progress;
}

uint32_t InstallProgress::Total()
{
    return m_total;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
