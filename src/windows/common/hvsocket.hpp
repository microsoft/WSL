/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    hvsocket.hpp

Abstract:

    This file contains hvsocket helper function declarations.

--*/

#pragma once

#include <hvsocket.h>
#include <wil/resource.h>

namespace wsl::windows::common::hvsocket {

constexpr ULONG c_connectTimeoutMs = 30 * 1000;
wil::unique_socket Accept(_In_ SOCKET ListenSocket, _In_ int Timeout, _In_opt_ HANDLE ExitHandle = nullptr);

wil::unique_socket Connect(_In_ const GUID& VmId, _In_ unsigned long Port, _In_opt_ HANDLE ExitHandle = nullptr, ULONG Timeout = c_connectTimeoutMs);

wil::unique_socket Create();

wil::unique_socket Listen(_In_ const GUID& VmId, _In_ unsigned long Port, _In_ int Backlog = -1);

} // namespace wsl::windows::common::hvsocket
