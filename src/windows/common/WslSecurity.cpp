/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslSecurity.cpp

Abstract:

    This file contains WSL Core security function definitions.

--*/

#include "precomp.h"
#include "WslSecurity.h"

std::unique_ptr<wsl::windows::common::security::privilege_context> wsl::windows::common::security::AcquirePrivilege(_In_ LPCWSTR privilegeName)
{
    // Open the token of the current process.
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(::OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token));

    auto luid = EnableTokenPrivilege(token.get(), privilegeName);
    return std::make_unique<wsl::windows::common::security::privilege_context>(std::move(token), luid);
}

std::vector<std::unique_ptr<wsl::windows::common::security::privilege_context>> wsl::windows::common::security::AcquirePrivileges(
    _In_ const std::vector<LPCWSTR>& privilegeNames)
{
    std::vector<std::unique_ptr<wsl::windows::common::security::privilege_context>> context;
    for (const auto* name : privilegeNames)
    {
        context.emplace_back(AcquirePrivilege(name));
    }

    return context;
}

void wsl::windows::common::security::ApplyProcessMitigationPolicies()
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY codePolicy{};
    codePolicy.AllowRemoteDowngrade = false;
    codePolicy.AllowThreadOptOut = false;
    codePolicy.ProhibitDynamicCode = true;
    LOG_IF_WIN32_BOOL_FALSE(SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &codePolicy, sizeof(codePolicy)));

    // Note: Enabling PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY::DisallowWin32kSystemCalls
    // breaks the service initialization logic (CoInitializeSecurity fails).

    PROCESS_MITIGATION_FONT_DISABLE_POLICY fontPolicy{};
    fontPolicy.DisableNonSystemFonts = true;
    LOG_IF_WIN32_BOOL_FALSE(SetProcessMitigationPolicy(ProcessFontDisablePolicy, &fontPolicy, sizeof(fontPolicy)));

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY loadPolicy{};
    loadPolicy.PreferSystem32Images = true;
    LOG_IF_WIN32_BOOL_FALSE(SetProcessMitigationPolicy(ProcessImageLoadPolicy, &loadPolicy, sizeof(loadPolicy)));
}

SECURITY_DESCRIPTOR wsl::windows::common::security::CreateSecurityDescriptor(_In_ PSID userSid)
{
    SECURITY_DESCRIPTOR sd{};
    THROW_IF_WIN32_BOOL_FALSE(InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION));
    THROW_IF_WIN32_BOOL_FALSE(SetSecurityDescriptorOwner(&sd, userSid, false));
    return sd;
}

wil::unique_handle wsl::windows::common::security::CreateRestrictedToken(_In_ HANDLE token)
{
    // N.B. These operations must be done while impersonating the user to avoid
    //      accidentally raising the integrity level.
    auto runAsUser = wil::impersonate_token(token);

    // Get the thread token with appropriate access rights.
    wil::unique_handle newToken{};
    THROW_IF_WIN32_BOOL_FALSE(::OpenThreadToken(
        ::GetCurrentThread(), (TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY), TRUE, &newToken));

    // Create a restricted token with only the SeChangeNotifyPrivilege privilege.
    wil::unique_handle restrictedToken{};
    THROW_IF_WIN32_BOOL_FALSE(::CreateRestrictedToken(newToken.get(), DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &restrictedToken));

    // Drop the token down to medium integrity level.
    union
    {
        SID sid;
        BYTE buffer[SECURITY_SID_SIZE(1)];
    } sidBuffer;
    SID_IDENTIFIER_AUTHORITY systemSidAuthority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    THROW_IF_NTSTATUS_FAILED(::RtlInitializeSidEx(&sidBuffer.sid, &systemSidAuthority, 1, SECURITY_MANDATORY_MEDIUM_RID));

    // Set the integrity level to untrusted.
    TOKEN_MANDATORY_LABEL tokenLabel{};
    tokenLabel.Label.Attributes = SE_GROUP_INTEGRITY;
    tokenLabel.Label.Sid = &sidBuffer.sid;
    THROW_IF_WIN32_BOOL_FALSE(::SetTokenInformation(
        restrictedToken.get(), TokenIntegrityLevel, &tokenLabel, (sizeof(tokenLabel) + ::GetLengthSid(&sidBuffer.sid))));

    return restrictedToken;
}

LUID wsl::windows::common::security::EnableTokenPrivilege(_Inout_ HANDLE token, _In_ LPCWSTR privilegeName)
{
    // Convert privilege name to an LUID.
    LUID luid{};
    THROW_IF_WIN32_BOOL_FALSE(::LookupPrivilegeValueW(nullptr, privilegeName, &luid));

    TOKEN_PRIVILEGES newState{};
    newState.PrivilegeCount = 1;
    newState.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    newState.Privileges[0].Luid = luid;
    THROW_IF_WIN32_BOOL_FALSE(::AdjustTokenPrivileges(token, FALSE, &newState, 0, nullptr, nullptr));

    return luid;
}

DWORD wsl::windows::common::security::GetUserBasicIntegrityLevel(_In_ HANDLE token)
{
    // Get the integrity level.
    const auto label = wil::get_token_information<TOKEN_MANDATORY_LABEL>(token);
    DWORD BasicIntegrityLevel =
        *GetSidSubAuthority(label->Label.Sid, static_cast<UCHAR>(*::GetSidSubAuthorityCount(label->Label.Sid) - 1));

    // Convert the range of medium integrity level to a single level.
    if ((BasicIntegrityLevel >= SECURITY_MANDATORY_MEDIUM_RID) && (BasicIntegrityLevel < SECURITY_MANDATORY_HIGH_RID))
    {
        BasicIntegrityLevel = SECURITY_MANDATORY_MEDIUM_RID;
    }

    return BasicIntegrityLevel;
}

bool wsl::windows::common::security::IsTokenElevated(_In_ HANDLE token)
{
    return (GetUserBasicIntegrityLevel(token) == SECURITY_MANDATORY_HIGH_RID);
}

wil::unique_handle wsl::windows::common::security::GetUserToken(_In_ TOKEN_TYPE tokenType, _In_ RPC_BINDING_HANDLE handle)
{
    wil::unique_handle contextToken;

    // Start by impersonating the caller and getting their token-user data out.
    {
        std::variant<int, wil::unique_coreverttoself_call, unique_revert_to_self> runAsClient;

        if (handle == nullptr)
        {
            runAsClient = wil::CoImpersonateClient();
        }
        else
        {
            runAsClient = RpcImpersonateCaller(handle);
        }

        THROW_IF_WIN32_BOOL_FALSE(::OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE | TOKEN_READ, TRUE, &contextToken));
    }

    wil::unique_handle newToken;
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateTokenEx(
        contextToken.get(), TOKEN_DUPLICATE | TOKEN_IMPERSONATE | TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, nullptr, SecurityImpersonation, tokenType, &newToken));

    return newToken;
}

bool wsl::windows::common::security::IsTokenLocalSystem(_In_opt_ HANDLE token)
{
    auto [sid, sidBuffer] = wsl::windows::common::security::CreateSid(SECURITY_NT_AUTHORITY, SECURITY_LOCAL_SYSTEM_RID);

    BOOL member{};
    THROW_IF_WIN32_BOOL_FALSE(::CheckTokenMembership(token, sid, &member));

    return member ? true : false;
}

wsl::windows::common::security::unique_revert_to_self wsl::windows::common::security::RpcImpersonateCaller(_In_ RPC_BINDING_HANDLE handle)
{
    THROW_IF_WIN32_ERROR(static_cast<DWORD>(RpcImpersonateClient(handle)));

    return {};
}
