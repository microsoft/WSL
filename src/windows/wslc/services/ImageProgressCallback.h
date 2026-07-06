/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageProgressCallback.h

Abstract:

    This file contains the ImageProgressCallback definition

--*/
#pragma once
#include "Reporter.h"
#include "SessionService.h"
#include "VTSupport.h"
#include <map>
#include <optional>
#include <string>

namespace wsl::windows::wslc::services {

// TODO: Handle terminal resizes.
class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") ImageProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    explicit ImageProgressCallback(Reporter& reporter) : m_reporter(reporter)
    {
    }
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    auto MoveToLine(int line);
    void WriteTerminal(std::wstring_view content) const;
    std::wstring GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, std::optional<int> visibleWidth);
    Reporter& m_reporter;
    std::map<std::string, int> m_statuses;
    int m_currentLine = 0;
    // Captured once: the progress display only renders when the Info channel is a VT console.
    bool m_vtEnabled = m_reporter.IsVTEnabled(Reporter::Level::Info);
};
} // namespace wsl::windows::wslc::services
