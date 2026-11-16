// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wsl::windows::service::wslc {

struct ComposeEnvironmentFile
{
    std::filesystem::path SourcePath;
    std::string Content;
};

using ComposeVariableLookup = std::function<std::optional<std::string>(std::string_view)>;

// Replaces $VAR and ${VAR...} references in value; sourcePath identifies the file in error messages.
std::string InterpolateComposeValue(std::string_view value, const ComposeVariableLookup& lookup, const std::filesystem::path& sourcePath);

// Variables that resolve references in compose files and environment entries without a value.
class ComposeVariables
{
public:
    ComposeVariables() = default;

    // Environment files apply in order, and process environment entries override every file.
    ComposeVariables(const std::vector<std::string>& processEnvironment, const std::vector<ComposeEnvironmentFile>& files);

    std::optional<std::string> Find(std::string_view name) const;
    std::string Interpolate(std::string_view value, const std::filesystem::path& sourcePath) const;

private:
    std::map<std::string, std::string, std::less<>> m_variables;
};

} // namespace wsl::windows::service::wslc
