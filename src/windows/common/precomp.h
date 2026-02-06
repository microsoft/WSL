/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    precomp.h

Abstract:

    The precompiled header for common.

--*/

#pragma warning(disable : 4200 4214)

#ifdef __cplusplus
#define _WINSOCKAPI_

// System headers
#include <windows.h>
#include <winapifamily.h>
#include <wincon.h>
#include <initguid.h>
#include <sddl.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2spi.h>
#include <wdk.h>
#include <FileApi.h>
#include <userenv.h>
#include <appmodel.h>
#include <virtdisk.h>
#include <conio.h>
#include <VersionHelpers.h>
#include <KnownFolders.h>
#include <shtypes.h>
#include <Shlwapi.h>
#include <Shlobj.h>
#include <icu.h>
#define ENABLE_INTSAFE_SIGNED_FUNCTIONS
#include <intsafe.h>
#include <safeint.h>
#include <windns.h>
#include <ipifcons.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <ip2string.h>
#include <atlsafe.h> // Note: needs to be included before tchar.h
#include <SoftPub.h>
#include <wintrust.h>
#include <msi.h>
#include <AccCtrl.h>
#include <AclAPI.h>
#include "windowsdefs.h"

// Annotations
#include <sal.h>

// Standard library C-style
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>

// Standard library C++ style
#include <string>
#include <array>
#include <xstring>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>
#include <locale>
#include <fstream>
#include <streambuf>
#include <string_view>
#include <thread>
#include <bitset>
#include <sstream>
#include <optional>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <codecvt>
#include <random>
#include <future>
#include <unordered_set>
#include <queue>
#include <numeric>
#include <regex>
#include <source_location>
#include <ranges>
#include <format>
#include <cwctype>
#include <variant>

// Socket APIs
#include <mswsock.h>
#include <ws2tcpip.h>
#include <hvsocket.h>
#include <wininet.h>
#include <mstcpip.h>

// Windows Internal Library
#include <wil/resource.h>
#include <wil/result.h>
#include <wil/filesystem.h>
#include <wil/token_helpers.h>
#include <wil/stl.h>
#include <wil/cppwinrt.h>
#include <wil/winrt.h>
#include <wil/wrl.h>
#include <wil/registry.h>

// Winrt headers
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/windows.web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.ApplicationModel.Store.Preview.InstallControl.h>
#include <winrt/Windows.Services.Store.h>
#include <winrt/Windows.Storage.h>

// Safe path handling
#include <pathcch.h>

// Telemetry Header
#include "WslTelemetry.h"

// LxCore headers
#include <lxcoreapi.h>
#include <lxbusapi.h>

// Utility/helper functions
#include "conncheckshared.h"
#include "helpers.hpp"
#include "string.hpp"
#include "filesystem.hpp"
#include "Localization.h"
#include "wslutil.h"
#include "gslhelpers.h"
#include "socket.hpp"
#include "defs.h"
#include "configfile.h"
#include "socketshared.h"
#include "stringshared.h"
#include "retryshared.h"
#include "wslconfig.h"
#include "Redirector.h"
#include "hvsocket.hpp"
#include "registry.hpp"
#include "LxssDynamicFunction.h"
#include "relay.hpp"
#include "svccomm.hpp"
#include "ConsoleState.h"
#include "disk.hpp"
#include "WslSecurity.h"
#include "ExecutionContext.h"
#include "WslClient.h"
#include "wslsupport.h"
#include <wslservice.h>
#include "message.h"
#include "wslpolicies.h"
#include "SubProcess.h"
#include "notifications.h"
#include "SocketChannel.h"
#include "interop.hpp"
#include "lxssbusclient.h"
#include "lxssclient.h"
#include "lxinitshared.h"
#include "wslhost.h"
#include "wslrelay.h"
#include "wsl.h"
#include "LxssPort.h"

#endif
