/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wsl.h

Abstract:

    This file contains command line arguments for wsl.exe.

--*/

#pragma once

#define WSL_BINARY_NAME L"wsl.exe"
#define WSL_CHANGE_DIRECTORY_ARG L"--cd"
#define WSL_CWD_HOME L"~"
#define WSL_DEBUG_SHELL_ARG_LONG L"--debug-shell"
#define WSL_DISTRO_ARG L"-d"
#define WSL_DISTRO_ARG_LONG L"--distribution"
#define WSL_DISTRIBUTION_ID_ARG L"--distribution-id"
#define WSL_EXEC_ARG L"-e"
#define WSL_EXEC_ARG_LONG L"--exec"
#define WSL_EXPORT_ARG L"--export"
#define WSL_EXPORT_ARG_STDOUT L"-"
#define WSL_EXPORT_ARG_FORMAT_OPTION L"--format"
#define WSL_EXPORT_ARG_VHD_OPTION L"--vhd"
#define WSL_HELP_ARG L"--help"
#define WSL_IMPORT_ARG L"--import"
#define WSL_IMPORT_ARG_STDIN L"-"
#define WSL_IMPORT_ARG_VERSION L"--version"
#define WSL_IMPORT_ARG_VHD L"--vhd"
#define WSL_IMPORT_INPLACE_ARG L"--import-in-place"
#define WSL_INSTALL_ARG L"--install"
#define WSL_INSTALL_ARG_DIST_OPTION L'd'
#define WSL_INSTALL_ARG_DIST_OPTION_LONG L"--distribution"
#define WSL_INSTALL_ARG_ENABLE_WSL1_LONG L"--enable-wsl1"
#define WSL_INSTALL_ARG_FIXED_VHD L"--fixed-vhd"
#define WSL_INSTALL_ARG_FROM_FILE_OPTION L'f'
#define WSL_INSTALL_ARG_FROM_FILE_LONG L"--from-file"
#define WSL_INSTALL_ARG_LEGACY_LONG L"--legacy"
#define WSL_INSTALL_ARG_LOCATION_OPTION L'l'
#define WSL_INSTALL_ARG_LOCATION_LONG L"--location"
#define WSL_INSTALL_ARG_NAME_LONG L"--name"
#define WSL_INSTALL_ARG_NO_LAUNCH_OPTION L'n'
#define WSL_INSTALL_ARG_NO_DISTRIBUTION_OPTION L"--no-distribution"
#define WSL_INSTALL_ARG_NO_LAUNCH_OPTION_LONG L"--no-launch"
#define WSL_INSTALL_ARG_VHD_SIZE L"--vhd-size"
#define WSL_INSTALL_ARG_VERSION L"--version"
#define WSL_INSTALL_ARG_WEB_DOWNLOAD_LONG L"--web-download"
#define WSL_INSTALL_ARG_PRERELEASE_LONG L"--pre-release"
#define WSL_INSTALL_ARG_PROMPT_BEFORE_EXIT_OPTION L"--prompt-before-exit"
#define WSL_LIST_ARG L"-l"
#define WSL_LIST_ARG_LONG L"--list"
#define WSL_LIST_ARG_ALL_OPTION L"--all"
#define WSL_LIST_ARG_ONLINE_OPTION L'o'
#define WSL_LIST_ARG_ONLINE_OPTION_LONG L"--online"
#define WSL_LIST_ARG_QUIET_OPTION L'q'
#define WSL_LIST_ARG_QUIET_OPTION_LONG L"--quiet"
#define WSL_LIST_ARG_RUNNING_OPTION L"--running"
#define WSL_LIST_ARG_VERBOSE_OPTION L'v'
#define WSL_LIST_ARG_VERBOSE_OPTION_LONG L"--verbose"
#define WSL_LIST_HEADER_FRIENDLY_NAME L"FRIENDLY NAME"
#define WSL_LIST_HEADER_NAME L"NAME"
#define WSL_LIST_HEADER_STATE L"STATE"
#define WSL_LIST_HEADER_VERSION L"VERSION"
#define WSL_MANAGE_ARG L"--manage"
#define WSL_MANAGE_ARG_ALLOW_UNSAFE L"--allow-unsafe"
#define WSL_MANAGE_ARG_MOVE_OPTION L'm'
#define WSL_MANAGE_ARG_MOVE_OPTION_LONG L"--move"
#define WSL_MANAGE_ARG_RESIZE_OPTION L'r'
#define WSL_MANAGE_ARG_RESIZE_OPTION_LONG L"--resize"
#define WSL_MANAGE_ARG_SET_SPARSE_OPTION L's'
#define WSL_MANAGE_ARG_SET_SPARSE_OPTION_LONG L"--set-sparse"
#define WSL_MANAGE_ARG_SET_DEFAULT_USER_OPTION_LONG L"--set-default-user"
#define WSL_MOUNT_ARG L"--mount"
#define WSL_MOUNT_ARG_VHD_OPTION_LONG L"--vhd"
#define WSL_MOUNT_ARG_BARE_OPTION_LONG L"--bare"
#define WSL_MOUNT_ARG_NAME_OPTION_LONG L"--name"
#define WSL_MOUNT_ARG_TYPE_OPTION_LONG L"--type"
#define WSL_MOUNT_ARG_OPTIONS_OPTION_LONG L"--options"
#define WSL_MOUNT_ARG_PARTITION_OPTION_LONG L"--partition"
#define WSL_MOUNT_ARG_BARE_OPTION L'b'
#define WSL_MOUNT_ARG_NAME_OPTION L'n'
#define WSL_MOUNT_ARG_TYPE_OPTION L't'
#define WSL_MOUNT_ARG_OPTIONS_OPTION L'o'
#define WSL_MOUNT_ARG_PARTITION_OPTION L'p'
#define WSL_PARENT_CONSOLE_ARG L"--parent-console"
#define WSL_SET_DEFAULT_DISTRO_ARG L"-s"
#define WSL_SET_DEFAULT_DISTRO_ARG_LEGACY L"--setdefault"
#define WSL_SET_DEFAULT_DISTRO_ARG_LONG L"--set-default"
#define WSL_SET_DEFAULT_VERSION_ARG L"--set-default-version"
#define WSL_SET_VERSION_ARG L"--set-version"
#define WSL_SHELL_OPTION_ARG L"--shell-type"
#define WSL_SHELL_OPTION_ARG_LOGIN_OPTION L"login"
#define WSL_SHELL_OPTION_ARG_NOSHELL_OPTION L"none"
#define WSL_SHELL_OPTION_ARG_STANDARD_OPTION L"standard"
#define WSL_SHUTDOWN_ARG L"--shutdown"
#define WSL_SHUTDOWN_OPTION_FORCE L"--force"
#define WSL_STATUS_ARG L"--status"
#define WSL_STOP_PARSING_ARG L"--"
#define WSL_SYSTEM_DISTRO_ARG L"--system"
#define WSL_TERMINATE_ARG L"-t"
#define WSL_TERMINATE_ARG_LONG L"--terminate"
#define WSL_UNINSTALL_ARG L"--uninstall"
#define WSL_UNMOUNT_ARG L"--unmount"
#define WSL_UNREGISTER_ARG L"--unregister"
#define WSL_UPDATE_ARG L"--update"
#define WSL_UPDATE_ARG_CONFIRM_OPTION_LONG L"--confirm"
#define WSL_UPDATE_ARG_PRE_RELEASE_OPTION_LONG L"--pre-release"
#define WSL_UPDATE_ARG_PROMPT_OPTION_LONG L"--prompt-before-exit"
#define WSL_UPDATE_ARG_WEB_DOWNLOAD_OPTION_LONG L"--web-download"
#define WSL_USER_ARG L"-u"
#define WSL_USER_ARG_LONG L"--user"
#define WSL_VERSION_ARG L"-v"
#define WSL_VERSION_ARG_LONG L"--version"
