// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ExecutionContext.h"
#include "WSLCDiagnostics.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

// Extends COMServiceExecutionContext with a cached diagnostic reporter and cancellable
// reverse COM calls for diagnostics.
class WSLCExecutionContext : public wsl::windows::common::COMServiceExecutionContext
{
public:
    NON_COPYABLE(WSLCExecutionContext);
    NON_MOVABLE(WSLCExecutionContext);

    WSLCExecutionContext(WSLCSession* session, IDiagnosticCallback* diagnosticCallback = nullptr) :
        m_diagnostics(diagnosticCallback, [session](const std::function<HRESULT()>& callback) {
            if (session == nullptr)
            {
                return callback();
            }

            auto comCallback = session->RegisterUserCOMCallback();
            return callback();
        })
    {
    }

    ~WSLCExecutionContext() override = default;

    const wsl::windows::wslc::diagnostics::DiagnosticReporter& Diagnostics() const noexcept
    {
        return m_diagnostics;
    }

protected:
    bool CollectUserWarning(const std::wstring& warning) override
    {
        if (m_diagnostics.IsCallbackReady())
        {
            if (!m_diagnostics.IsEnabled(WSLCDiagnosticLevelWarning))
            {
                return true;
            }

            const auto result = m_diagnostics.TryReport(WSLCDiagnosticLevelWarning, WSLC_DIAG_CODE_USER_WARNING, warning.c_str());
            if (SUCCEEDED(result) || result == RPC_E_CALL_CANCELED || result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                return true;
            }

            LOG_HR(result);
        }

        return COMServiceExecutionContext::CollectUserWarning(warning);
    }

private:
    wsl::windows::wslc::diagnostics::DiagnosticReporter m_diagnostics;
};

} // namespace wsl::windows::service::wslc
