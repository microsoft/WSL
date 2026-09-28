/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslhost.h

Abstract:

    Header file containing command line arguments for wslhost.exe.

--*/

#pragma once

namespace wslhost {

LPCWSTR const binary_name = L"wslhost.exe";

// Arguments for NotificationActivator::Activate entrypoint.
LPCWSTR const disable_notification_arg = L"--disable-notification";
LPCWSTR const docs_arg = L"--docs";
LPCWSTR const docs_arg_filesystem_url = L"https://aka.ms/wslfilesystemdocs";
LPCWSTR const event_viewer_arg = L"--event-viewer";
LPCWSTR const install_prerequisites_arg = L"--install-prerequisites";
LPCWSTR const release_notes_arg = L"--release-notes";
LPCWSTR const update_arg = L"--update";

// Arguments for wWinMain entrypoint.
LPCWSTR const distro_id_option = L"--distro-id";
LPCWSTR const handle_option = L"--handle";
LPCWSTR const event_option = L"--event";
LPCWSTR const parent_option = L"--parent";
LPCWSTR const vm_id_option = L"--vm-id";
LPCWSTR const embedding_option = L"-Embedding";
} // namespace wslhost