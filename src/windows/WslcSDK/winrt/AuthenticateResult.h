// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include "Microsoft.WSL.Containers.AuthenticateResult.g.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct AuthenticateResult : AuthenticateResultT<AuthenticateResult>
{
    AuthenticateResult(hstring identityToken, winrt::Microsoft::WSL::Containers::IdentityTokenType tokenType);

    hstring IdentityToken();
    winrt::Microsoft::WSL::Containers::IdentityTokenType TokenType();

private:
    hstring m_identityToken{};
    winrt::Microsoft::WSL::Containers::IdentityTokenType m_tokenType{};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

DEFINE_TYPE_HELPERS(AuthenticateResult);
