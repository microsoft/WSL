/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VhdOptions.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK VhdOptions class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.VhdOptions.g.h"
#include "Helpers.h"
#include "Defaults.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct VhdOptions : VhdOptionsT<VhdOptions>
{
    VhdOptions() = default;

    VhdOptions(hstring const& name, uint64_t size, winrt::Microsoft::WSL::Containers::VhdType const& type);
    hstring Name();
    void Name(hstring const& value);
    uint64_t Size();
    void Size(uint64_t value);
    winrt::Microsoft::WSL::Containers::VhdType Type();
    void Type(winrt::Microsoft::WSL::Containers::VhdType const& value);

    winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::VhdOwner> Owner();
    void Owner(winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::VhdOwner> const& value);

    WslcVhdRequirements* ToStructPointer();

private:
    std::string m_name{};
    uint64_t m_size{s_DefaultStorageSize};
    winrt::Microsoft::WSL::Containers::VhdType m_type{winrt::Microsoft::WSL::Containers::VhdType::Dynamic};
    winrt::Windows::Foundation::IReference<winrt::Microsoft::WSL::Containers::VhdOwner> m_owner{nullptr};

    std::unique_ptr<WslcVhdRequirements> m_vhdOptions{nullptr};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct VhdOptions : VhdOptionsT<VhdOptions, implementation::VhdOptions>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(VhdOptions);