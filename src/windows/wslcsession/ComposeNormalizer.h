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
    std::vector<std::byte> Content;
};

struct ComposeDocuments
{
    ULONG SchemaVersion{};
    std::filesystem::path WorkingDirectory;
    std::filesystem::path ProjectDirectory;
    std::optional<std::string> ExplicitProjectName;
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
    static bool IsValidProjectKey(std::string_view projectKey) noexcept;
    static std::string ValidateProjectKey(std::string_view projectKey);
    static void ValidateSelection(const ComposeProjectSelection& selection);
};

} // namespace wsl::windows::service::wslc
