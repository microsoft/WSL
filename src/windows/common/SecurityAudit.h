// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <authz.h>
#include <wil/resource.h>
#include <span>

namespace wsl::windows::common::securityaudit {

bool Enabled();
void Uninstall();

class EventSource
{
public:
    EventSource();
    NON_COPYABLE(EventSource);
    NON_MOVABLE(EventSource);

    void Report(DWORD eventId, std::span<std::wstring> fields);

private:
    static void Close(AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE provider) noexcept;
    wil::unique_handle m_token;
    wil::unique_any<AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE, decltype(&Close), Close> m_provider;
};

} // namespace wsl::windows::common::securityaudit
