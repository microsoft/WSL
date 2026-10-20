/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkInfo.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK NetworkInfo class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.NetworkInfo.g.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct NetworkInfo : NetworkInfoT<NetworkInfo>
{
    NetworkInfo() = default;
    NetworkInfo(WslcNetworkInfo const& info);

    hstring Id();
    hstring Name();
    hstring Driver();
    hstring Scope();
    winrt::Windows::Foundation::DateTime CreatedTimestamp();
    bool EnableIPv4();
    bool EnableIPv6();
    bool Internal();

private:
    hstring m_id{};
    hstring m_name{};
    hstring m_driver{};
    hstring m_scope{};
    winrt::Windows::Foundation::DateTime m_createdTimestamp{};
    bool m_enableIPv4{};
    bool m_enableIPv6{};
    bool m_internal{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(NetworkInfo);
