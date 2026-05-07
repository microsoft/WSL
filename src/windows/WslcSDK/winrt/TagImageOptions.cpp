/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TagImageOptions.cpp

Abstract:

    This file contains the implementation of the WinRT wrapper for the WSLC SDK TagImageOptions class.

--*/

#include "precomp.h"
#include "TagImageOptions.h"
#include "Microsoft.WSL.Containers.TagImageOptions.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
TagImageOptions::TagImageOptions(hstring const& image, hstring const& repository, hstring const& tag)
{
    throw hresult_not_implemented();
}
hstring TagImageOptions::Image()
{
    throw hresult_not_implemented();
}
void TagImageOptions::Image(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring TagImageOptions::Repository()
{
    throw hresult_not_implemented();
}
void TagImageOptions::Repository(hstring const& value)
{
    throw hresult_not_implemented();
}
hstring TagImageOptions::Tag()
{
    throw hresult_not_implemented();
}
void TagImageOptions::Tag(hstring const& value)
{
    throw hresult_not_implemented();
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
