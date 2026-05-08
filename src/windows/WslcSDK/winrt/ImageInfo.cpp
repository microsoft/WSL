/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageInfo.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ImageInfo class.

--*/

#include "precomp.h"
#include "ImageInfo.h"
#include "Microsoft.WSL.Containers.ImageInfo.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
ImageInfo::ImageInfo(WslcImageInfo const& info) : m_info(info)
{
}

hstring ImageInfo::Name()
{
    return winrt::to_hstring(m_info.name);
}

winrt::Windows::Storage::Streams::IBuffer ImageInfo::Sha256()
{
    winrt::Windows::Storage::Streams::DataWriter writer;
    writer.WriteBytes(m_info.sha256);
    return writer.DetachBuffer();
}

uint64_t ImageInfo::SizeBytes()
{
    return m_info.sizeBytes;
}

winrt::Windows::Foundation::DateTime ImageInfo::CreatedTimestamp()
{
    return winrt::clock::from_time_t(static_cast<time_t>(m_info.createdUnixTime));
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
