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
#include <string>

namespace wsl::windows::wslc::services {

// TODO: Handle terminal resizes.
class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") ImageProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    // level selects the target stream: Output (stdout) for standalone pull/push, Info (stderr) for
    // the implicit pull during run/create, matching Docker.
    ImageProgressCallback(Reporter& reporter, Reporter::Level level) : m_reporter(reporter), m_level(level)
    {
    }
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    auto MoveToLine(int line);
    std::wstring GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, int visibleWidth);
    Reporter& m_reporter;
    // Declared before m_vtEnabled, whose initializer reads it.
    const Reporter::Level m_level;
    std::map<std::string, int> m_statuses;
    // Last status text per id; used only when redirected to dedupe repeated byte-progress callbacks.
    std::map<std::string, std::string> m_lastStatusById;
    int m_currentLine = 0;
    // The progress display only renders on a VT console.
    bool m_vtEnabled = m_reporter.IsVTEnabled(m_level);
};
} // namespace wsl::windows::wslc::services
