// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Terminal.h"
#include "WSLCEvent.h"
#include <wslc.h>
#include <wslutil.h>

namespace wsl::windows::wslc::services {

using namespace wsl::windows::wslc::cli;

class DECLSPEC_UUID("20D009D8-D80C-42F8-826C-9DD91738A518") DiagnosticCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDiagnosticCallback, IFastRundown>
{
public:
    explicit DiagnosticCallback(Terminal& terminal) : m_terminal(terminal)
    {
    }

    HRESULT GetEnabledLevels(WSLCDiagnosticLevel* Levels) override
    {
        RETURN_HR_IF_NULL(E_POINTER, Levels);

        *Levels = WSLCDiagnosticLevelWarning;
        if (m_terminal.IsDebugEnabled())
        {
            *Levels |= WSLCDiagnosticLevelDebug;
        }

        return S_OK;
    }

    HRESULT OnDiagnostic(const WSLCDiagnosticEvent* Event) override
    try
    {
        RETURN_HR_IF_NULL(E_POINTER, Event);
        RETURN_HR_IF(E_INVALIDARG, Event->SchemaVersion != WSLC_DIAGNOSTIC_SCHEMA_VERSION);
        RETURN_HR_IF(E_INVALIDARG, Event->Code == nullptr || Event->Code[0] == '\0');

        switch (Event->Level)
        {
        case WSLCDiagnosticLevelDebug:
            if (m_terminal.IsDebugEnabled())
            {
                m_terminal.Debug(
                    L"{}",
                    wsl::windows::wslc::events::FormatDebugEvent(
                        Event->Code, Event->Message != nullptr ? std::wstring_view{Event->Message} : std::wstring_view{}, Event->Timestamp));
            }
            break;

        case WSLCDiagnosticLevelWarning:
            RETURN_HR_IF_NULL(E_INVALIDARG, Event->Message);
            m_terminal.Warn(L"{}", Event->Message);
            break;

        default:
            return E_INVALIDARG;
        }

        return S_OK;
    }
    CATCH_RETURN()

private:
    Terminal& m_terminal;
};

} // namespace wsl::windows::wslc::services
