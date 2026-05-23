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

    VhdOptions(hstring const& name, uint64_t sizeInBytes, winrt::Microsoft::WSL::Containers::VhdType const& type);
    hstring Name();
    void Name(hstring const& value);
    uint64_t SizeInBytes();
    void SizeInBytes(uint64_t value);
    winrt::Microsoft::WSL::Containers::VhdType Type();
    void Type(winrt::Microsoft::WSL::Containers::VhdType const& value);

    void SetOwner(uint32_t uid, uint32_t gid);

    WslcVhdRequirements* ToStructPointer();

private:
    std::string m_name;
    uint64_t m_sizeInBytes = s_DefaultStorageSize;
    winrt::Microsoft::WSL::Containers::VhdType m_type = winrt::Microsoft::WSL::Containers::VhdType::Dynamic;
    std::optional<std::pair<uint32_t, uint32_t>> m_owner;

    std::unique_ptr<WslcVhdRequirements> m_vhdOptions;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct VhdOptions : VhdOptionsT<VhdOptions, implementation::VhdOptions>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(VhdOptions);