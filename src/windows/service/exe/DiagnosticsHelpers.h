/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagnosticsHelpers.h

Abstract:

    Helper functions for distribution start failure diagnostics.

--*/

#pragma once

#include "precomp.h"

namespace wsl::windows::service::diagnostics {

// Maps a create-instance failure step to a human-readable localized description.
// Exposed (non-anonymous) so it can be unit-tested.
inline std::wstring CreateInstanceStepDescription(_In_ LX_MINI_CREATE_INSTANCE_STEP step)
{
    switch (step)
    {
    case LxInitCreateInstanceStepFormatDisk:
        return wsl::shared::Localization::MessageCreateInstanceStepFormatDisk();

    case LxInitCreateInstanceStepMountDisk:
        return wsl::shared::Localization::MessageCreateInstanceStepMountDisk();

    case LxInitCreateInstanceStepLaunchSystemDistro:
        return wsl::shared::Localization::MessageCreateInstanceStepLaunchDistribution();

    case LxInitCreateInstanceStepRunTar:
        return wsl::shared::Localization::MessageCreateInstanceStepRunTar();

    default:
        return wsl::shared::Localization::MessageCreateInstanceStepUnknown();
    }
}

} // namespace wsl::windows::service::diagnostics
