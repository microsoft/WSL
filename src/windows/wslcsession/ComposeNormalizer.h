// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ComposeSpec.h"
#include "wslc.h"
#include <optional>

namespace wsl::windows::service::wslc {

struct ComposeDocument
{
    std::filesystem::path SourcePath;
    std::filesystem::path BaseDirectory;
    // Original encoded bytes define the snapshot interpreted by parsing and normalization.
    std::vector<std::byte> Content;
};

struct ComposeDocuments
{
    std::filesystem::path ProjectDirectory;
    std::optional<std::string> ExplicitProjectName;
    // The collection retains input order and per-document source context.
    std::vector<ComposeDocument> Documents;
};

struct ComposeProjectSelection
{
    std::vector<std::string> Profiles;
    std::vector<std::string> Services;
    bool IncludeDependencies{};
};

struct ComposeNormalizer
{
    static ComposeSpec Normalize(const ComposeDocuments& documents, const ComposeProjectSelection& selection);
    static std::string DefaultNetworkName(std::string_view projectName);
    static bool IsValidProjectKey(std::string_view projectKey) noexcept;
    static std::string ValidateProjectKey(std::string_view projectKey);
    static void ValidateSelection(const ComposeProjectSelection& selection);
};

} // namespace wsl::windows::service::wslc
