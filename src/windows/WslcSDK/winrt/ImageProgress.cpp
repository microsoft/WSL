/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageProgress.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK ImageProgress class.

--*/

#include "precomp.h"
#include "ImageProgress.h"
#include "Microsoft.WSL.Containers.ImageProgress.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
hstring ImageProgress::Id()
{
    return m_id;
}

void ImageProgress::Id(hstring const& value)
{
    m_id = value;
}

winrt::Microsoft::WSL::Containers::ImageProgressStatus ImageProgress::Status()
{
    return m_status;
}

void ImageProgress::Status(winrt::Microsoft::WSL::Containers::ImageProgressStatus const& value)
{
    m_status = value;
}

uint64_t ImageProgress::CurrentBytes()
{
    return m_currentBytes;
}

void ImageProgress::CurrentBytes(uint64_t value)
{
    m_currentBytes = value;
}

uint64_t ImageProgress::TotalBytes()
{
    return m_totalBytes;
}

void ImageProgress::TotalBytes(uint64_t value)
{
    m_totalBytes = value;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
