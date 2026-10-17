// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "RestrictedUserToken.hpp"

#include <Windows.h>

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

class TokenHandle final
{
public:
    TokenHandle() noexcept = default;
    explicit TokenHandle(HANDLE handle) noexcept : handle_(handle)
    {
    }
    ~TokenHandle()
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(handle_);
        }
    }

    TokenHandle(const TokenHandle&) = delete;
    TokenHandle& operator=(const TokenHandle&) = delete;

    HANDLE Get() const noexcept
    {
        return handle_;
    }
    void Reset(HANDLE handle) noexcept
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
        {
            (void)CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

bool QueryTokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS informationClass, std::vector<std::uint8_t>& buffer)
{
    DWORD required = 0U;
    (void)GetTokenInformation(token, informationClass, nullptr, 0U, &required);
    if (required == 0U)
    {
        return false;
    }
    buffer.resize(required);
    return GetTokenInformation(token, informationClass, buffer.data(), required, &required) != FALSE;
}

bool SameUser(HANDLE first, HANDLE second)
{
    std::vector<std::uint8_t> firstBuffer;
    std::vector<std::uint8_t> secondBuffer;
    if (!QueryTokenBuffer(first, TokenUser, firstBuffer) || !QueryTokenBuffer(second, TokenUser, secondBuffer))
    {
        return false;
    }
    const auto* firstUser = reinterpret_cast<const TOKEN_USER*>(firstBuffer.data());
    const auto* secondUser = reinterpret_cast<const TOKEN_USER*>(secondBuffer.data());
    return IsValidSid(firstUser->User.Sid) != FALSE && IsValidSid(secondUser->User.Sid) != FALSE &&
           EqualSid(firstUser->User.Sid, secondUser->User.Sid) != FALSE;
}

bool SameSession(HANDLE first, HANDLE second)
{
    DWORD firstSession = 0U;
    DWORD secondSession = 0U;
    DWORD bytesReturned = 0U;
    return GetTokenInformation(first, TokenSessionId, &firstSession, sizeof(firstSession), &bytesReturned) != FALSE &&
           bytesReturned == sizeof(firstSession) &&
           GetTokenInformation(second, TokenSessionId, &secondSession, sizeof(secondSession), &bytesReturned) != FALSE &&
           bytesReturned == sizeof(secondSession) && firstSession == secondSession;
}

bool SameIntegrity(HANDLE first, HANDLE second)
{
    std::vector<std::uint8_t> firstBuffer;
    std::vector<std::uint8_t> secondBuffer;
    if (!QueryTokenBuffer(first, TokenIntegrityLevel, firstBuffer) || !QueryTokenBuffer(second, TokenIntegrityLevel, secondBuffer))
    {
        return false;
    }
    const auto* firstLabel = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(firstBuffer.data());
    const auto* secondLabel = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(secondBuffer.data());
    return IsValidSid(firstLabel->Label.Sid) != FALSE && IsValidSid(secondLabel->Label.Sid) != FALSE &&
           EqualSid(firstLabel->Label.Sid, secondLabel->Label.Sid) != FALSE && firstLabel->Label.Attributes == secondLabel->Label.Attributes;
}

bool SamePrivileges(HANDLE first, HANDLE second)
{
    std::vector<std::uint8_t> firstBuffer;
    std::vector<std::uint8_t> secondBuffer;
    if (!QueryTokenBuffer(first, TokenPrivileges, firstBuffer) || !QueryTokenBuffer(second, TokenPrivileges, secondBuffer))
    {
        return false;
    }
    const auto* firstPrivileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(firstBuffer.data());
    const auto* secondPrivileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(secondBuffer.data());
    if (firstPrivileges->PrivilegeCount != secondPrivileges->PrivilegeCount)
    {
        return false;
    }

    LUID changeNotifyPrivilege{};
    if (LookupPrivilegeValueW(nullptr, L"SeChangeNotifyPrivilege", &changeNotifyPrivilege) == FALSE)
    {
        return false;
    }
    for (DWORD i = 0U; i < firstPrivileges->PrivilegeCount; ++i)
    {
        const auto& firstPrivilege = firstPrivileges->Privileges[i];
        const auto& secondPrivilege = secondPrivileges->Privileges[i];
        if (firstPrivilege.Luid.LowPart != secondPrivilege.Luid.LowPart ||
            firstPrivilege.Luid.HighPart != secondPrivilege.Luid.HighPart || firstPrivilege.Attributes != secondPrivilege.Attributes)
        {
            return false;
        }
        const bool isChangeNotify = firstPrivilege.Luid.LowPart == changeNotifyPrivilege.LowPart &&
                                    firstPrivilege.Luid.HighPart == changeNotifyPrivilege.HighPart;
        if ((firstPrivilege.Attributes & SE_PRIVILEGE_ENABLED) != 0U && !isChangeNotify)
        {
            return false;
        }
    }
    return true;
}

bool CheckPrimary(HANDLE token)
{
    TOKEN_TYPE type{};
    DWORD bytesReturned = 0U;
    return GetTokenInformation(token, TokenType, &type, sizeof(type), &bytesReturned) != FALSE && bytesReturned == sizeof(type) &&
           type == TokenPrimary;
}

bool PreservesRestrictedIdentity(HANDLE restrictedToken, HANDLE primaryToken)
{
    return CheckPrimary(primaryToken) && SameUser(restrictedToken, primaryToken) && SameSession(restrictedToken, primaryToken) &&
           SameIntegrity(restrictedToken, primaryToken) && SamePrivileges(restrictedToken, primaryToken);
}

bool CreateRestrictedPrimary(HANDLE source, TokenHandle& restricted)
{
    HANDLE token = nullptr;
    if (CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 0U, nullptr, 0U, nullptr, 0U, nullptr, &token) == FALSE)
    {
        return false;
    }
    restricted.Reset(token);
    return CheckPrimary(restricted.Get());
}

bool TestInvalidHandles()
{
    using wsl::virtio_sound::native::windows::detail::DuplicateRestrictedUserTokenAsPrimary;
    if (DuplicateRestrictedUserTokenAsPrimary(nullptr) != nullptr || DuplicateRestrictedUserTokenAsPrimary(INVALID_HANDLE_VALUE) != nullptr)
    {
        std::cerr << "invalid token handles were accepted\n";
        return false;
    }
    return true;
}

bool TestPrimaryToken(HANDLE restrictedPrimary)
{
    using wsl::virtio_sound::native::windows::detail::DuplicateRestrictedUserTokenAsPrimary;
    TokenHandle converted(DuplicateRestrictedUserTokenAsPrimary(restrictedPrimary));
    if (converted.Get() == nullptr || !PreservesRestrictedIdentity(restrictedPrimary, converted.Get()))
    {
        std::cerr << "restricted primary token conversion failed or changed token state\n";
        return false;
    }
    return true;
}

bool TestImpersonationTokens(HANDLE restrictedPrimary)
{
    using wsl::virtio_sound::native::windows::detail::DuplicateRestrictedUserTokenAsPrimary;
    constexpr DWORD c_impersonationAccess = TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE;

    HANDLE impersonation = nullptr;
    if (DuplicateTokenEx(restrictedPrimary, c_impersonationAccess, nullptr, SecurityImpersonation, TokenImpersonation, &impersonation) == FALSE)
    {
        std::cerr << "unable to create restricted impersonation test token\n";
        return false;
    }
    TokenHandle restrictedImpersonation(impersonation);
    TokenHandle converted(DuplicateRestrictedUserTokenAsPrimary(restrictedImpersonation.Get()));
    if (converted.Get() == nullptr || !PreservesRestrictedIdentity(restrictedImpersonation.Get(), converted.Get()))
    {
        std::cerr << "restricted impersonation token conversion failed or changed token state\n";
        return false;
    }

    HANDLE identification = nullptr;
    if (DuplicateTokenEx(restrictedPrimary, c_impersonationAccess, nullptr, SecurityIdentification, TokenImpersonation, &identification) == FALSE)
    {
        std::cerr << "unable to create restricted identification test token\n";
        return false;
    }
    TokenHandle restrictedIdentification(identification);
    if (DuplicateRestrictedUserTokenAsPrimary(restrictedIdentification.Get()) != nullptr)
    {
        std::cerr << "identification-level token was accepted\n";
        return false;
    }
    return true;
}

} // namespace

int main()
{
    TokenHandle processToken;
    HANDLE processTokenValue = nullptr;
    constexpr DWORD c_processTokenAccess = TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT;
    if (OpenProcessToken(GetCurrentProcess(), c_processTokenAccess, &processTokenValue) == FALSE)
    {
        std::cerr << "unable to open current process token\n";
        return 1;
    }
    processToken.Reset(processTokenValue);

    TokenHandle restrictedPrimary;
    if (!CreateRestrictedPrimary(processToken.Get(), restrictedPrimary))
    {
        std::cerr << "unable to create restricted primary test token\n";
        return 1;
    }

    if (!TestInvalidHandles() || !TestPrimaryToken(restrictedPrimary.Get()) || !TestImpersonationTokens(restrictedPrimary.Get()))
    {
        return 1;
    }

    std::cout << "PASS: restricted primary/impersonation token conversion; "
                 "identification-level and invalid tokens rejected; user, session, "
                 "integrity, and disabled privileges preserved.\n";
    return 0;
}
