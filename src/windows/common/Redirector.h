/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Redirector.h

Abstract:

    This file contains declarations for helpers for controlling the Plan 9 Redirector.

--*/

#pragma once

namespace wsl::windows::common::redirector {

class ConnectionTargetManager final
{
public:
    ConnectionTargetManager(std::wstring_view name);

    void AddConnectionTarget(
        HANDLE userToken, std::string_view aname, LX_UID_T uid, std::wstring_view unixSocketPath = {}, const GUID& instanceId = {}, ULONG port = 0);

    void RemoveAll();

private:
    bool Contains(LUID luid) const;

    std::wstring_view m_name;
    std::vector<LUID> m_logonIds;
    wil::srwlock m_lock;
};

wil::unique_hfile OpenRedirector();

void EnsureRedirectorStarted();

bool StartRedirector();

void AddConnectionTarget(
    std::wstring_view name, LUID logonId, std::string_view aname, LX_UID_T uid, std::wstring_view unixSocketPath, const GUID& instanceId = {}, ULONG port = 0);

void RemoveConnectionTarget(std::wstring_view name, LUID logonId);

void RegisterUserCallback(HANDLE handle, gsl::span<gsl::byte> outputBuffer, LPOVERLAPPED overlapped);

} // namespace wsl::windows::common::redirector
