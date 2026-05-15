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
winrt::Microsoft::WSL::Containers::ComponentFlags InstallProgress::Component()
{
    throw hresult_not_implemented();
}
uint32_t InstallProgress::Progress()
{
    throw hresult_not_implemented();
}
uint32_t InstallProgress::Total()
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
