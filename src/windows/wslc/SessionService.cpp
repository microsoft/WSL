#include "precomp.h"
#include "SessionService.h"
#include <wslaservice.h>

namespace wslc::services
{
using namespace wslc::models;

DEFINE_ENUM_FLAG_OPERATORS(WSLASessionFlags);

Session SessionService::CreateSession(std::optional<SessionOptions> options)
{
    const SessionOptions& sessionOptions = options.has_value() ? options.value() : SessionOptions::Default();
    wil::com_ptr<IWSLASessionManager> sessionManager;
    THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());
    auto dataFolder = std::filesystem::path(wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr)) / "wsla";
    wil::com_ptr<IWSLASession> session;
   THROW_IF_FAILED(sessionManager->CreateSession(&options.value(), WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting, &session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
    return Session(std::move(session));
}
}