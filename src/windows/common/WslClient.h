/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslClient.h

Abstract:

    This file contains the declaration for WSL client entry points.

--*/

#pragma once

namespace wsl::windows::common {
class WslClient
{
public:
    static int Main(_In_ LPCWSTR commandLine);
};
} // namespace wsl::windows::common
