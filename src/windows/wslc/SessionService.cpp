/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionService.cpp

Abstract:

    This file contains the SessionService implementation

--*/
#include "precomp.h"
#include "SessionService.h"
#include <wslaservice.h>

namespace wslc::services {
using namespace wslc::models;

DEFINE_ENUM_FLAG_OPERATORS(WSLASessionFlags);

Session SessionService::CreateSession(std::optional<SessionOptions> options)
{
    SessionOptions sessionOptions = options.has_value() ? options.value() : SessionOptions::Default();
    const WSLA_SESSION_SETTINGS* settings = sessionOptions;
    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    wil::com_ptr<IWSLASession> session;
    THROW_IF_FAILED(sessionManager->CreateSession(settings, WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}
} // namespace wslc::services