// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "AuthenticateResult.h"
#include "Microsoft.WSL.Containers.AuthenticateResult.g.cpp"

namespace winrt::Microsoft::WSL::Containers::implementation {
AuthenticateResult::AuthenticateResult(hstring identityToken, winrt::Microsoft::WSL::Containers::IdentityTokenType tokenType) :
    m_identityToken(std::move(identityToken)), m_tokenType(tokenType)
{
}

hstring AuthenticateResult::IdentityToken()
{
    return m_identityToken;
}

winrt::Microsoft::WSL::Containers::IdentityTokenType AuthenticateResult::TokenType()
{
    return m_tokenType;
}
} // namespace winrt::Microsoft::WSL::Containers::implementation
