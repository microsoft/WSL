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

namespace winrt::Microsoft::WSL::Containers::implementation {
struct VhdRequirements : VhdRequirementsT<VhdRequirements>
{
    VhdRequirements() = default;

    VhdRequirements(hstring const& name, uint64_t sizeInBytes, winrt::Microsoft::WSL::Containers::VhdType const& type);
    hstring Name();
    uint64_t SizeInBytes();
    winrt::Microsoft::WSL::Containers::VhdType Type();

    WslcVhdRequirements* ToStructPointer();

private:
    std::string m_name;
    WslcVhdRequirements m_vhdRequirements{nullptr};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct VhdRequirements : VhdRequirementsT<VhdRequirements, implementation::VhdRequirements>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(VhdRequirements);