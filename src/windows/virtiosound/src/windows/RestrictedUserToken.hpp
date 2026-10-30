// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <wil/resource.h>

namespace wsl::virtio_sound::native::windows::detail {

// Convert a caller-supplied restricted user token into the primary token
// required by CreateProcessAsUserW. The caller retains ownership of input;
// ownership of a non-null result transfers to the caller.
inline HANDLE DuplicateRestrictedUserTokenAsPrimary(HANDLE restrictedUserToken) noexcept
{
    if (restrictedUserToken == nullptr || restrictedUserToken == INVALID_HANDLE_VALUE)
    {
        return nullptr;
    }

    TOKEN_TYPE inputTokenType{};
    DWORD tokenTypeBytes = 0U;
    if (GetTokenInformation(restrictedUserToken, TokenType, &inputTokenType, sizeof(inputTokenType), &tokenTypeBytes) == FALSE ||
        tokenTypeBytes != sizeof(inputTokenType) || (inputTokenType != TokenPrimary && inputTokenType != TokenImpersonation))
    {
        return nullptr;
    }

    if (inputTokenType == TokenImpersonation)
    {
        SECURITY_IMPERSONATION_LEVEL impersonationLevel{};
        DWORD impersonationLevelBytes = 0U;
        if (GetTokenInformation(restrictedUserToken, TokenImpersonationLevel, &impersonationLevel, sizeof(impersonationLevel), &impersonationLevelBytes) ==
                FALSE ||
            impersonationLevelBytes != sizeof(impersonationLevel) || impersonationLevel < SecurityImpersonation)
        {
            return nullptr;
        }
    }

    HANDLE primaryTokenRaw = nullptr;
    const BOOL duplicateSucceeded = DuplicateTokenEx(
        restrictedUserToken, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, nullptr, SecurityImpersonation, TokenPrimary, &primaryTokenRaw);
    wil::unique_handle primaryToken{primaryTokenRaw};
    if (duplicateSucceeded == FALSE)
    {
        return nullptr;
    }

    TOKEN_TYPE outputTokenType{};
    DWORD outputTokenTypeBytes = 0U;
    if (GetTokenInformation(primaryToken.get(), TokenType, &outputTokenType, sizeof(outputTokenType), &outputTokenTypeBytes) == FALSE ||
        outputTokenTypeBytes != sizeof(outputTokenType) || outputTokenType != TokenPrimary)
    {
        return nullptr;
    }

    return primaryToken.release();
}

} // namespace wsl::virtio_sound::native::windows::detail
