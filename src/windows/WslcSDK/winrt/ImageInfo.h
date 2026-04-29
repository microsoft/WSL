/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageInfo.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK ImageInfo class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.ImageInfo.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct ImageInfo : ImageInfoT<ImageInfo>
{
    ImageInfo() = default;
    ImageInfo(WslcImageInfo const& info);

    hstring Name();
    winrt::Windows::Storage::Streams::IBuffer Sha256();
    uint64_t SizeBytes();
    winrt::Windows::Foundation::DateTime CreatedTimestamp();

private:
    WslcImageInfo m_info{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(ImageInfo);
