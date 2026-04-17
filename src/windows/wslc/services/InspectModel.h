/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InspectModel.h

Abstract:

    This file contains the InspectModel definition

--*/
#pragma once

namespace wsl::windows::wslc::models {
typedef enum _InspectType
{
    Container = 1,
    Image = 2,
    All = Container | Image,
} InspectType;
} // namespace wsl::windows::wslc::models
