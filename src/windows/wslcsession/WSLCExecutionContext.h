// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ExecutionContext.h"
#include "WSLCDiagnostics.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

// Extends COMServiceExecutionContext with a WSLCSession pointer for lazy COM callback
// registration when warnings are emitted. This enables EMIT_USER_WARNING to stream
// warnings back to the CLI via IDiagnosticCallback, with proper cancellation support
// during session termination via RegisterUserCOMCallback/CoCancelCall.
class WSLCExecutionContext : public wsl::windows::common::COMServiceExecutionContext
{
public:
    NON_COPYABLE(WSLCExecutionContext);
    NON_MOVABLE(WSLCExecutionContext);

    WSLCExecutionContext(WSLCSession* session, IDiagnosticCallback* diagnosticCallback = nullptr) :
        m_session(session), m_diagnostics(diagnosticCallback)
    {
    }

    ~WSLCExecutionContext() override = default;

protected:
    bool CollectUserWarning(const std::wstring& warning) override
    {
        if (m_diagnostics.HasCallback())
        {
            if (!m_diagnostics.IsEnabled(WSLCDiagnosticLevelWarning))
            {
                return true;
            }

            std::unique_ptr<UserCOMCallback> comCallback;
            if (m_session != nullptr)
            {
                comCallback = std::make_unique<UserCOMCallback>(m_session->RegisterUserCOMCallback());
            }

            WSLC_DIAG(m_diagnostics, WSLCDiagnosticLevelWarning, WSLC_DIAG_CODE_USER_WARNING, L"{}", warning);
            return true;
        }

        return COMServiceExecutionContext::CollectUserWarning(warning);
    }

private:
    WSLCSession* m_session = nullptr;
    wsl::windows::wslc::diagnostics::DiagnosticReporter m_diagnostics;
};

} // namespace wsl::windows::service::wslc
