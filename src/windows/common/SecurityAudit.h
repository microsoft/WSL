// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <authz.h>
#include <wil/resource.h>
#include <span>
#include <string_view>
#include <nlohmann/json_fwd.hpp>

namespace wsl::windows::common::securityaudit {

bool Enabled();
void Uninstall();

std::string EscapeEventParameters(std::string_view text);
std::wstring FormatEventField(const nlohmann::json& object, const char* name);
std::wstring NormalizeEventMessage(std::wstring_view text);
std::wstring FormatSyscallEventDisplay(std::string_view syscall, std::wstring_view text);

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
