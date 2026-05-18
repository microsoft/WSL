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
ImageInfo::ImageInfo(WslcImageInfo const& info)
{
    m_name = winrt::to_hstring(info.name);
    m_sizeBytes = info.sizeBytes;
    m_createdTimestamp = winrt::clock::from_time_t(static_cast<time_t>(info.createdUnixTime));

    winrt::Windows::Storage::Streams::DataWriter writer;
    writer.WriteBytes(info.sha256);
    m_sha256 = writer.DetachBuffer();
}

hstring ImageInfo::Name()
{
    return m_name;
}

winrt::Windows::Storage::Streams::IBuffer ImageInfo::Sha256()
{
    return m_sha256;
}

uint64_t ImageInfo::SizeBytes()
{
    return m_sizeBytes;
}

winrt::Windows::Foundation::DateTime ImageInfo::CreatedTimestamp()
{
    return m_createdTimestamp;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
