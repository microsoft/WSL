// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ExecutionContext.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

// Extends COMServiceExecutionContext with a WSLCSession pointer for lazy COM callback
// registration when warnings are emitted. This enables EMIT_USER_WARNING to stream
// warnings back to the CLI via IWarningCallback, with proper cancellation support
// during session termination via RegisterUserCOMCallback/CoCancelCall.
class WSLCExecutionContext : public wsl::windows::common::COMServiceExecutionContext
{
public:
    NON_COPYABLE(WSLCExecutionContext);
    NON_MOVABLE(WSLCExecutionContext);

    WSLCExecutionContext(WSLCSession* session, IWarningCallback* warningCallback = nullptr) :
        m_session(session), m_warningCallback(warningCallback)
    {
    }

    ~WSLCExecutionContext() override = default;

protected:
    bool CollectUserWarning(const std::wstring& warning) override
    {
        if (m_warningCallback != nullptr)
        {
            std::unique_ptr<UserCOMCallback> comCallback;
            if (m_session != nullptr)
            {
                comCallback = std::make_unique<UserCOMCallback>(m_session->RegisterUserCOMCallback());
            }

            auto hr = m_warningCallback->OnWarning(warning.c_str());
            if (SUCCEEDED(hr) || hr == RPC_E_CALL_CANCELED || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                return true;
            }

            LOG_HR(hr);
        }

        return COMServiceExecutionContext::CollectUserWarning(warning);
    }

private:
    WSLCSession* m_session = nullptr;
    IWarningCallback* m_warningCallback = nullptr;
};

} // namespace wsl::windows::service::wslc
