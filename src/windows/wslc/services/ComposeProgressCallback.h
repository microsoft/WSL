// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Terminal.h"
#include "wslc.h"

namespace wsl::windows::wslc::services {

class DECLSPEC_UUID("6A2033FE-B0DD-4CBD-8EA6-7D6628D9AF9A") ComposeProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IComposeProgressCallback, IFastRundown>
{
public:
    explicit ComposeProgressCallback(Terminal& Terminal) noexcept : m_terminal(Terminal)
    {
    }

    IFACEMETHOD(OnProgress)(_In_ const WSLCComposeProgressEvent* Event) override;
    IFACEMETHOD(OnStreamsReady)(_In_ const WSLCComposeStreams* Streams) override;

private:
    Terminal& m_terminal;
};

} // namespace wsl::windows::wslc::services
