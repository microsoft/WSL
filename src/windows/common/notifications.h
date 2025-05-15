/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    notifications.h

Abstract:

    This file contains notification function definitions.

--*/

#pragma once

namespace wsl::windows::common::notifications {

/// <summary>
/// Displays the notification that a WSL update is available.
/// </summary>
HRESULT DisplayUpdateNotification(const std::wstring& versionString);

/// <summary>
/// Displays the notification that performance will be poor due to DrvFs usage.
/// </summary>
HRESULT DisplayFilesystemNotification(_In_ LPCSTR binaryName);

/// <summary>
/// Displays the notification saying that warnings were emitted during launch.
/// </summary>
void DisplayWarningsNotification();

/// <summary>
/// Displays the notification saying that a proxy change has been detected.
/// </summary>
void DisplayProxyChangeNotification(const std::wstring& message);

/// <summary>
/// Displays the notification saying that optional components need to be installed.
/// </summary>
void DisplayOptionalComponentsNotification();

} // namespace wsl::windows::common::notifications
