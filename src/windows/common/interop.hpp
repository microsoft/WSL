/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    interop.hpp

Abstract:

    This file contains interop function declarations.

--*/

#pragma once

#include <winsock2.h>
#include "GuestConnector.h"

namespace wsl::windows::common::interop {

void WorkerThread(_In_ wil::unique_handle&& ServerPortHandle);

DWORD VmModeWorkerThread(_In_ wsl::shared::SocketChannel& channel, _In_ const GuestConnector& Connector, _In_ bool IgnoreExit = false);

} // namespace wsl::windows::common::interop
