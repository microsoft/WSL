/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageProgress.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ImageProgress class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ImageProgress.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ImageProgress : ImageProgressT<ImageProgress>
{
    ImageProgress() = default;
    ImageProgress(const WslcImageProgressMessage* progress);

    hstring Id();
    void Id(hstring const& value);
    winrt::Microsoft::WSL::Containers::ImageProgressStatus Status();
    void Status(winrt::Microsoft::WSL::Containers::ImageProgressStatus const& value);
    uint64_t CurrentBytes();
    void CurrentBytes(uint64_t value);
    uint64_t TotalBytes();
    void TotalBytes(uint64_t value);

private:
    hstring m_id;
    winrt::Microsoft::WSL::Containers::ImageProgressStatus m_status{};
    uint64_t m_currentBytes{};
    uint64_t m_totalBytes{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(ImageProgress);
