/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSecurity.h

Abstract:

    This file contains WSL Core security function declarations.

--*/

#pragma once

#include <xstring>
#include <optional>
#include "wrl/client.h"
#include "wil/resource.h"
#include "Redirector.h"

namespace wsl::windows::common::security {
using unique_revert_to_self = wil::unique_call<decltype(&::RpcRevertToSelf), ::RpcRevertToSelf>;

using unique_acl = wil::unique_any<ACL*, decltype(&::LocalFree), ::LocalFree>;

struct privilege_context
{
    privilege_context() = delete;
    privilege_context(const privilege_context&) = delete;

    privilege_context(wil::unique_handle&& token, LUID luid) : token(std::move(token)), luid(luid)
    {
    }

    ~privilege_context()
    {
        // Disable the privilege.
        if (token)
        {
            TOKEN_PRIVILEGES newState{};
            newState.PrivilegeCount = 1;
            newState.Privileges[0].Attributes = 0;
            newState.Privileges[0].Luid = luid;
            LOG_IF_WIN32_BOOL_FALSE(::AdjustTokenPrivileges(token.get(), FALSE, &newState, 0, nullptr, nullptr));
        }
    }

    wil::unique_handle token;
    LUID luid;
};

/// <summary>
/// Acquires the specified privilege on the current process token.
/// </summary>
std::unique_ptr<privilege_context> AcquirePrivilege(_In_ LPCWSTR privilegeName);

/// <summary>
/// Acquires the specified privileges on the current process token.
/// </summary>
std::vector<std::unique_ptr<privilege_context>> AcquirePrivileges(_In_ const std::vector<LPCWSTR>& privilegeNames);

/// <summary>
/// Apply process mitigation policies to current process.
/// </summary>
void ApplyProcessMitigationPolicies();

/// <summary>
/// Creates a security descriptor from the provided user sid.
/// </summary>
SECURITY_DESCRIPTOR CreateSecurityDescriptor(_In_ PSID userSid);

template <typename... TArgs>
std::pair<PSID, std::vector<char>> CreateSid(SID_IDENTIFIER_AUTHORITY Authority, TArgs... values)
{
    std::vector<char> buffer(SECURITY_SID_SIZE(sizeof...(TArgs)));
    auto* sid = reinterpret_cast<PSID>(buffer.data());

    THROW_IF_NTSTATUS_FAILED(RtlInitializeSidEx(sid, &Authority, sizeof...(TArgs), std::forward<TArgs>(values)...));

    return std::make_pair(sid, std::move(buffer));
}

/// <summary>
/// Creates a restricted token from the provided token.
/// </summary>
wil::unique_handle CreateRestrictedToken(_In_ HANDLE token);

/// <summary>
/// Enables a privilege on the token.
/// </summary>
LUID EnableTokenPrivilege(_Inout_ HANDLE token, _In_ LPCWSTR privilegeName);

/// <summary>
/// Returns the basic integrity level for provided token.
/// </summary>
DWORD GetUserBasicIntegrityLevel(_In_ HANDLE token);

/// <summary>
/// Returns the user token for the current client.
/// </summary>
wil::unique_handle GetUserToken(_In_ TOKEN_TYPE tokenType, _In_ RPC_BINDING_HANDLE handle = nullptr);

/// <summary>
/// Queries if the provided token is elevated.
/// </summary>
bool IsTokenElevated(_In_ HANDLE token);

/// <summary>
/// Returns true if the provided token is a member of the localsystem group
/// </summary>
bool IsTokenLocalSystem(_In_opt_ HANDLE token);

/// <summary>
/// Impersonates the RPC caller
/// </summary>
unique_revert_to_self RpcImpersonateCaller(_In_ RPC_BINDING_HANDLE handle);
} // namespace wsl::windows::common::security
