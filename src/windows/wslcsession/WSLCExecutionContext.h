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
            // Lazily register for COM cancellation on first warning so
            // CancelUserCOMCallbacks() can abort the OnWarning call during session termination.
            // Skip registration if this thread is already registered (e.g., by IProgressCallback
            // in StreamImageOperation), since the thread is already cancellable.
            if (!m_comCallback.has_value() && m_session != nullptr && !m_session->IsUserCOMCallbackRegistered())
            {
                m_comCallback = m_session->RegisterUserCOMCallback();
            }

            LOG_IF_FAILED(m_warningCallback->OnWarning(warning.c_str()));
            return true;
        }

        return COMServiceExecutionContext::CollectUserWarning(warning);
    }

private:
    WSLCSession* m_session = nullptr;
    IWarningCallback* m_warningCallback = nullptr;
    std::optional<UserCOMCallback> m_comCallback;
};

} // namespace wsl::windows::service::wslc
