// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Reporter.h"
#include <wslc.h>
#include <wslutil.h>

namespace wsl::windows::wslc::services {

// Adapts the service IWarningCallback COM sink onto the CLI Reporter so the CLI, not the
// service, decides how warnings are presented (mirrors ImageProgressCallback).
class DECLSPEC_UUID("A7E3F8B2-4D19-4C6A-9E5B-8F2A1D3C7E90") WarningCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWarningCallback, IFastRundown>
{
public:
    explicit WarningCallback(Reporter& reporter) : m_reporter(reporter)
    {
    }

    HRESULT OnWarning(LPCWSTR Message) override
    {
        try
        {
            WI_ASSERT(Message);
            if (Message != nullptr)
            {
                // Message already carries the "wsl: " prefix and trailing newline from EmitUserWarning;
                // Warn writes it verbatim, adding color only on a VT console.
                m_reporter.Warn(L"{}", Message);
            }

            return S_OK;
        }
        CATCH_RETURN();
    }

private:
    Reporter& m_reporter;
};

} // namespace wsl::windows::wslc::services
