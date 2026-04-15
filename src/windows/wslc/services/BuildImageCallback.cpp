/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildImageCallback.cpp

Abstract:

    This file contains the BuildImageCallback Implementation.

--*/

#include "precomp.h"
#include "BuildImageCallback.h"

namespace wsl::windows::wslc::services {
HRESULT BuildImageCallback::OnProgress(LPCSTR status, LPCSTR /*id*/, ULONGLONG /*current*/, ULONGLONG /*total*/)
try
{
    if (status != nullptr && *status != '\0')
    {
        wprintf(L"%hs", status);
    }
    return S_OK;
}
CATCH_RETURN();
} // namespace wsl::windows::wslc::services
