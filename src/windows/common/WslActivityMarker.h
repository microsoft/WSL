/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslActivityMarker.h

Abstract:

    This file contains declarations for tracking whether WSL is in use.

--*/

#pragma once

namespace wsl::windows::common {

class WslActivityMarker
{
public:
    WslActivityMarker() noexcept;
    ~WslActivityMarker() noexcept;

    NON_COPYABLE(WslActivityMarker);
    NON_MOVABLE(WslActivityMarker);

    static bool IsWslActive() noexcept;
};

} // namespace wsl::windows::common
