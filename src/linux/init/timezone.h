/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    timezone.h

Abstract:

    This file contains timezone function declarations.

--*/

#pragma once

#include "WslDistributionConfig.h"

void UpdateTimezone(std::string_view Timezone, const wsl::linux::WslDistributionConfig& Config);

void UpdateTimezone(gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config);
