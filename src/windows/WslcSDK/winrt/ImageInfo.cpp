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
hstring ImageInfo::Name()
{
    throw hresult_not_implemented();
}
winrt::Windows::Storage::Streams::IBuffer ImageInfo::Sha256()
{
    throw hresult_not_implemented();
}
uint64_t ImageInfo::SizeBytes()
{
    throw hresult_not_implemented();
}
winrt::Windows::Foundation::DateTime ImageInfo::CreatedTimestamp()
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
