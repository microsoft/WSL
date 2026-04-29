/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VhdRequirements.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK VhdRequirements class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.VhdRequirements.g.h"
#include "Helpers.h"
#include "Defaults.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct VhdRequirements : VhdRequirementsT<VhdRequirements>
{
    VhdRequirements() = default;

    VhdRequirements(hstring const& name, uint64_t sizeInBytes, winrt::Microsoft::WSL::Containers::VhdType const& type);
    hstring Name();
    void Name(hstring const& value);
    uint64_t SizeInBytes();
    void SizeInBytes(uint64_t value);
    winrt::Microsoft::WSL::Containers::VhdType Type();
    void Type(winrt::Microsoft::WSL::Containers::VhdType const& value);

    WslcVhdRequirements* ToStructPointer();

private:
    std::string m_name;
    uint64_t m_sizeInBytes = s_DefaultStorageSize;
    winrt::Microsoft::WSL::Containers::VhdType m_type = winrt::Microsoft::WSL::Containers::VhdType::Dynamic;

    std::unique_ptr<WslcVhdRequirements> m_vhdRequirements;
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct VhdRequirements : VhdRequirementsT<VhdRequirements, implementation::VhdRequirements>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(VhdRequirements);