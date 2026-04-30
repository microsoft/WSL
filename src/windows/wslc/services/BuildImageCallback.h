/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildImageCallback.h

Abstract:

    This file contains the BuildImageCallback definition

--*/
#pragma once
#include "SessionService.h"

namespace wsl::windows::wslc::services {
class DECLSPEC_UUID("3EDD5DBF-CA6C-4CF7-923A-AD94B6A732E5") BuildImageCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;
};
} // namespace wsl::windows::wslc::services
