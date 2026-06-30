// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wslc.h>
#include <wslutil.h>
#include "Reporter.h"

namespace wsl::windows::wslc::services {

// COM callback invoked by the service once for each image loaded from a tar archive during
// 'wslc image load'. It surfaces each loaded image's name and tag to the CLI for display.
class DECLSPEC_UUID("91EF98A7-99A8-41C2-893C-43CDFB7DB69F") WSLCImageLoadCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IImageLoadCallback, IFastRundown>
{
public:
    // The reporter must outlive this callback.
    explicit WSLCImageLoadCallback(Reporter& reporter) : m_reporter(reporter)
    {
    }

    HRESULT OnImageLoaded(LPCSTR ImageName, EnumReferenceFormat Format) override;

private:
    Reporter& m_reporter;
};

} // namespace wsl::windows::wslc::services
