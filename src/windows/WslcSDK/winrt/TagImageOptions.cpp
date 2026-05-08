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

TagImageOptions::TagImageOptions(hstring const& image, hstring const& repository, hstring const& tag) :
    m_image(winrt::to_string(image)), m_repository(winrt::to_string(repository)), m_tag(winrt::to_string(tag))
{
    if (m_image.empty() || m_repository.empty() || m_tag.empty())
    {
        throw hresult_invalid_argument();
    }
}

hstring TagImageOptions::Image()
{
    return winrt::to_hstring(m_image);
}

void TagImageOptions::Image(hstring const& value)
{
    if (m_tagImageOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_image = winrt::to_string(value);
}

hstring TagImageOptions::Repository()
{
    return winrt::to_hstring(m_repository);
}

void TagImageOptions::Repository(hstring const& value)
{
    if (m_tagImageOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_repository = winrt::to_string(value);
}

hstring TagImageOptions::Tag()
{
    return winrt::to_hstring(m_tag);
}

void TagImageOptions::Tag(hstring const& value)
{
    if (m_tagImageOptions)
    {
        throw hresult_illegal_state_change();
    }

    if (value.empty())
    {
        throw hresult_invalid_argument();
    }

    m_tag = winrt::to_string(value);
}

WslcTagImageOptions* TagImageOptions::ToStructPointer()
{
    if (!m_tagImageOptions)
    {
        m_tagImageOptions = std::make_unique<WslcTagImageOptions>();
        m_tagImageOptions->image = m_image.c_str();
        m_tagImageOptions->repo = m_repository.c_str();
        m_tagImageOptions->tag = m_tag.c_str();
    }

    return m_tagImageOptions.get();
}

} // namespace winrt::Microsoft::WSL::Containers::implementation
