/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeModel.h

Abstract:

    This file contains the ComposeModel definitions

--*/

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace wsl::windows::wslc::models {

// Compose files to load, with the directory that relative paths in them resolve against.
struct ComposeProjectFiles
{
    std::vector<std::filesystem::path> Files;
    std::filesystem::path ProjectDirectory;
    std::optional<std::string> ProjectName;
};

// A compose project, identified either by its files or by the name of an existing project.
struct ComposeProject
{
    std::variant<ComposeProjectFiles, std::string> Value;
};

} // namespace wsl::windows::wslc::models
