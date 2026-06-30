// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCImageLoadCallback.h"

namespace wsl::windows::wslc::services {

using wsl::windows::common::string::MultiByteToWide;

HRESULT WSLCImageLoadCallback::OnImageLoaded(LPCSTR ImageName, EnumReferenceFormat Format)
try
{
    WI_ASSERT(ImageName != nullptr);

    if (Format == EnumReferenceFormatDigest)
    {
        m_reporter.Output(L"{}\n", wsl::shared::Localization::WSLCCLI_ImageLoadedId(MultiByteToWide(ImageName)));
    }
    else
    {
        m_reporter.Output(L"{}\n", wsl::shared::Localization::WSLCCLI_ImageLoaded(MultiByteToWide(ImageName)));
    }

    return S_OK;
}
CATCH_RETURN();

} // namespace wsl::windows::wslc::services
