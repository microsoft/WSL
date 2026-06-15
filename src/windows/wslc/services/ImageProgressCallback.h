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
#include <map>
#include <string>

namespace wsl::windows::wslc::services {

// TODO: Handle terminal resizes.
class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") ImageProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    explicit ImageProgressCallback(Reporter& output) : m_output(output)
    {
    }
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    auto MoveToLine(int line);
    void WriteTerminal(std::wstring_view content) const;
    std::wstring GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, std::optional<int> visibleWidth);
    Reporter& m_output;
    std::map<std::string, int> m_statuses;
    int m_currentLine = 0;
    bool m_vtEnabled = m_output.IsVTEnabled(Reporter::Level::Info);
    // Tracks which entries already had a status logged in plain mode so we only emit
    // transitions (e.g. "Pulling fs layer" -> "Download complete") instead of every byte update.
    std::map<std::string, std::string> m_plainStatuses;
};
} // namespace wsl::windows::wslc::services
