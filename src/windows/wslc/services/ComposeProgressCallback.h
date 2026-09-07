// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Terminal.h"
#include "wslc.h"

namespace wsl::windows::wslc::services {

class DECLSPEC_UUID("6A2033FE-B0DD-4CBD-8EA6-7D6628D9AF9A") ComposeProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IComposeProgressCallback, IFastRundown>
{
public:
    ComposeProgressCallback(Terminal& terminal, WSLCComposeAction action) noexcept : m_terminal(terminal), m_action(action)
    {
    }

    IFACEMETHOD(OnProgress)(_In_ const WSLCComposeProgressEvent* event) override;
    IFACEMETHOD(OnStreamsReady)(_In_ const WSLCComposeStreams* streams) override;

private:
    Terminal& m_terminal;
    const WSLCComposeAction m_action;
    bool m_removeProgressReported{};
};

} // namespace wsl::windows::wslc::services
