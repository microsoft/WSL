// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <winsock2.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <wil/resource.h>

namespace wsl::windows::common {

/// <summary>
/// Connects to guest ports without requiring the caller to know which virtual machine backend
/// is in use. HCS virtual machines are reached over AF_HYPERV; OpenVMM virtual machines are
/// reached through an AF_UNIX bridge socket.
///
/// N.B. This is an opaque, copyable handle because client processes (wsl.exe and wslhost.exe)
///      service interop requests out-of-process from the service and cannot call the backend
///      directly. Serialization is only used to carry the handle across those process boundaries.
/// </summary>
class GuestConnector
{
public:
    GuestConnector() = default;

    static GuestConnector CreateHcs(_In_ const GUID& VmId);
    static GuestConnector CreateOpenVmm(_In_ std::filesystem::path VsockPath);
    static GuestConnector Deserialize(_In_ std::wstring_view Value);

    wil::unique_socket Connect(_In_ unsigned long Port, _In_opt_ HANDLE ExitHandle = nullptr) const;
    std::wstring Serialize() const;

private:
    struct HcsConnection
    {
        GUID VmId{};
    };

    struct OpenVmmConnection
    {
        std::filesystem::path VsockPath;
    };

    explicit GuestConnector(HcsConnection Connection);
    explicit GuestConnector(OpenVmmConnection Connection);

    std::variant<std::monostate, HcsConnection, OpenVmmConnection> m_connection;
};

} // namespace wsl::windows::common
