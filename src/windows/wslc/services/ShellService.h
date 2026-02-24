/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ShellService.h

Abstract:

    This file contains the ShellService definition

--*/
#pragma once

#include <string>
#include <vector>

namespace wsl::windows::wslc::services {
struct SessionInformation
{
    ULONG SessionId;
    DWORD CreatorPid;
    std::wstring DisplayName;
};

class ShellService
{
public:
    int Attach(const std::wstring& name);
    std::vector<SessionInformation> List();
};
} // namespace wsl::windows::wslc::services
