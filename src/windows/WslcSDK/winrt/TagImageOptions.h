/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TagImageOptions.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK TagImageOptions class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.TagImageOptions.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct TagImageOptions : TagImageOptionsT<TagImageOptions>
{
    TagImageOptions() = default;

    TagImageOptions(hstring const& image, hstring const& repository, hstring const& tag);
    hstring Image();
    void Image(hstring const& value);
    hstring Repository();
    void Repository(hstring const& value);
    hstring Tag();
    void Tag(hstring const& value);

    WslcTagImageOptions* ToStructPointer();

private:
    std::string m_image;
    std::string m_repository;
    std::string m_tag;

    std::unique_ptr<WslcTagImageOptions> m_tagImageOptions;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct TagImageOptions : TagImageOptionsT<TagImageOptions, implementation::TagImageOptions>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(TagImageOptions);
