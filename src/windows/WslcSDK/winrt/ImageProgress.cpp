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
    throw hresult_not_implemented();
}
winrt::Microsoft::WSL::Containers::ImageProgressStatus ImageProgress::Status()
{
    throw hresult_not_implemented();
}
uint64_t ImageProgress::CurrentBytes()
{
    throw hresult_not_implemented();
}
uint64_t ImageProgress::TotalBytes()
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
