/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageProgress.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ImageProgress class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ImageProgress.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ImageProgress : ImageProgressT<ImageProgress>
{
    ImageProgress() = default;

    hstring Id();
    winrt::Microsoft::WSL::Containers::ImageProgressStatus Status();
    uint64_t CurrentBytes();
    uint64_t TotalBytes();
};
} // namespace winrt::Microsoft::WSL::Containers::implementation
