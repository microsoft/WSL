// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ComposeModel.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using wsl::windows::wslc::models::ComposeProjectInformation;

inline std::optional<ComposeProjectInformation> FindComposeProject(const std::vector<ComposeProjectInformation>& projects, const std::string& name)
{
    const auto project = std::ranges::find(projects, name, &ComposeProjectInformation::Name);
    return project == projects.end() ? std::nullopt : std::optional{*project};
}

inline bool ContainsComposeProject(const std::vector<ComposeProjectInformation>& projects, const std::string& name)
{
    return std::ranges::find(projects, name, &ComposeProjectInformation::Name) != projects.end();
}

inline void VerifyComposeProjectStatus(const std::wstring& projectName, const std::wstring& expectedStatus)
{
    const auto result = RunWslc(L"compose list --all --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    const auto projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
    const auto project = FindComposeProject(projects, wsl::shared::string::WideToMultiByte(projectName));
    VERIFY_IS_TRUE(project.has_value());
    VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(expectedStatus), project->Status);
}

inline void VerifyComposeProjectAbsent(const std::wstring& projectName)
{
    const auto result = RunWslc(L"compose list --all --format json");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    const auto projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
    VERIFY_IS_FALSE(ContainsComposeProject(projects, wsl::shared::string::WideToMultiByte(projectName)));
}

} // namespace WSLCE2ETests
