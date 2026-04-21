/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    precomp.h

Abstract:

    The precompiled header for WslcSDK/winrt.

--*/

#pragma once

#include "wslcsdk.h"

#include <wil/winrt.h>
#include <wil/resource.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include "Container.h"
#include "ContainerNamedVolume.h"
#include "ContainerPortMapping.h"
#include "ContainerSettings.h"
#include "ContainerVolume.h"
#include "ImageInfo.h"
#include "ImageProgress.h"
#include "InstallProgress.h"
#include "Process.h"
#include "ProcessSettings.h"
#include "PullImageOptions.h"
#include "PushImageOptions.h"
#include "ServiceVersion.h"
#include "Session.h"
#include "SessionSettings.h"
#include "TagImageOptions.h"
#include "VhdRequirements.h"
#include "WslcService.h"