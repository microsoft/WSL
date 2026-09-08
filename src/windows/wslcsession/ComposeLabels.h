// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

namespace wsl::windows::service::wslc::compose::label {

inline constexpr char c_project[] = "com.docker.compose.project";
inline constexpr char c_service[] = "com.docker.compose.service";
inline constexpr char c_containerNumber[] = "com.docker.compose.container-number";
inline constexpr char c_oneoff[] = "com.docker.compose.oneoff";
inline constexpr char c_network[] = "com.docker.compose.network";
inline constexpr char c_managed[] = "com.microsoft.wslc.compose.managed";
inline constexpr char c_managedValue[] = "true";
inline constexpr char c_metadataVersion[] = "com.microsoft.wslc.compose.metadata-version";
inline constexpr char c_metadataVersionValue[] = "1";

} // namespace wsl::windows::service::wslc::compose::label
