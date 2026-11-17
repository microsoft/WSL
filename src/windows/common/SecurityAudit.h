// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <authz.h>
#include <wil/resource.h>

namespace wsl::windows::common::securityaudit {

inline constexpr GUID c_processCreationAuditSubcategory{0x0cce922b, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}};
inline constexpr GUID c_applicationGeneratedAuditSubcategory{0x0cce9222, 0x69ae, 0x11d9, {0xbe, 0xd3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}};

bool Enabled();
void Uninstall();

class EventSource
{
public:
    EventSource();
    NON_COPYABLE(EventSource);
    NON_MOVABLE(EventSource);

    void ProcessExecuted(uint32_t pid);

private:
    static void Close(AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE provider) noexcept;
    wil::unique_handle m_token;
    wil::unique_any<AUTHZ_SECURITY_EVENT_PROVIDER_HANDLE, decltype(&Close), Close> m_provider;
};

} // namespace wsl::windows::common::securityaudit
