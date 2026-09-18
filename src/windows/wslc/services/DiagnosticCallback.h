// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Terminal.h"
#include <wslc.h>
#include <wslutil.h>

namespace wsl::windows::wslc::services {

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

        *Levels = WSLCDiagnosticLevelInformation | WSLCDiagnosticLevelWarning | WSLCDiagnosticLevelError;
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
        RETURN_HR_IF(E_POINTER, Event->Arguments.Count != 0 && Event->Arguments.Values == nullptr);

        switch (Event->Level)
        {
        case WSLCDiagnosticLevelDebug:
            if (m_terminal.IsDebugEnabled())
            {
                const auto code = wsl::shared::string::MultiByteToWide(Event->Code);
                if (Event->Message != nullptr)
                {
                    m_terminal.Debug(L"[{}] {}\n", code, Event->Message);
                }
                else
                {
                    m_terminal.Debug(L"[{}]\n", code);
                }
            }
            break;

        case WSLCDiagnosticLevelInformation:
            m_terminal.Info(L"{}", MessageOrCode(*Event));
            break;

        case WSLCDiagnosticLevelWarning:
            m_terminal.Warn(L"{}", MessageOrCode(*Event));
            break;

        case WSLCDiagnosticLevelError:
            m_terminal.Error(L"{}", MessageOrCode(*Event));
            break;

        default:
            return E_INVALIDARG;
        }

        return S_OK;
    }
    CATCH_RETURN()

private:
    static std::wstring MessageOrCode(const WSLCDiagnosticEvent& event)
    {
        if (event.Message != nullptr)
        {
            return event.Message;
        }

        return std::format(L"{}\n", wsl::shared::string::MultiByteToWide(event.Code));
    }
    Terminal& m_terminal;
};

} // namespace wsl::windows::wslc::services
