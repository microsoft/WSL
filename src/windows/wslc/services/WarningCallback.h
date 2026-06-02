// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wslc.h>
#include <wslutil.h>

namespace wsl::windows::wslc::services {

class DECLSPEC_UUID("A7E3F8B2-4D19-4C6A-9E5B-8F2A1D3C7E90") WarningCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWarningCallback, IFastRundown>
{
public:
    HRESULT OnWarning(LPCWSTR Message) override
    {
        WI_ASSERT(Message);
        wsl::windows::common::wslutil::PrintMessage(Message, stderr);
        return S_OK;
    }
};

} // namespace wsl::windows::wslc::services
