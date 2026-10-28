// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeNormalizer.h"
#include "ResourceName.h"
#include <set>

namespace wsl::windows::service::wslc {

namespace {

    std::string NormalizeProjectName(std::string_view name)
    {
        std::string result{name};
        std::ranges::transform(result, result.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        std::erase_if(result, [](unsigned char value) { return !std::isalnum(value) && value != '-' && value != '_'; });
        while (!result.empty() && !std::isalnum(static_cast<unsigned char>(result.front())))
        {
            result.erase(result.begin());
        }

        THROW_HR_IF(E_INVALIDARG, result.empty());
        THROW_HR_IF(E_INVALIDARG, result.size() > WSLC_MAX_COMPOSE_PROJECT_NAME_LENGTH);
        return result;
    }

} // namespace

ComposeSpec ComposeNormalizer::Normalize(const ComposeDocuments& documents, const ComposeProjectSelection& selection)
{
    THROW_HR_IF(E_INVALIDARG, documents.SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);
    THROW_HR_IF(E_INVALIDARG, documents.WorkingDirectory.empty());
    THROW_HR_IF(E_INVALIDARG, documents.ProjectDirectory.empty());
    THROW_HR_IF(E_INVALIDARG, documents.Documents.size() != 1);
    ValidateSelection(selection);

    const auto& document = documents.Documents.front();
    THROW_HR_IF(E_INVALIDARG, document.SourcePath.empty());
    THROW_HR_IF(E_INVALIDARG, document.BaseDirectory.empty());
    THROW_HR_IF(E_INVALIDARG, document.Content.empty());

    // yaml-cpp detects the document encoding from the original bytes and any byte-order mark.
    const std::string content(reinterpret_cast<const char*>(document.Content.data()), document.Content.size());
    auto spec = ComposeSpec::Parse(document.SourcePath, content);

    std::string projectName;
    if (documents.ExplicitProjectName.has_value())
    {
        projectName = NormalizeProjectName(*documents.ExplicitProjectName);
    }
    else
    {
        auto projectDirectory = documents.ProjectDirectory.lexically_normal();
        if (projectDirectory.filename().empty())
        {
            projectDirectory = projectDirectory.parent_path();
        }

        projectName = NormalizeProjectName(wsl::shared::string::WideToMultiByte(projectDirectory.filename().wstring()));
    }

    spec.ProjectName = std::move(projectName);
    ValidateName(DefaultNetworkName(spec.ProjectName).c_str(), WSLC_MAX_NETWORK_NAME_LENGTH);
    std::set<std::string> containerNames;
    for (auto& container : spec.Containers)
    {
        // Each service name is registered as a network alias, including when its container name is explicit.
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            wsl::shared::Localization::MessageWslcInvalidName(container.ServiceName.c_str()),
            std::ranges::all_of(container.ServiceName, [](unsigned char value) { return std::isspace(value); }));

        if (container.Name.empty())
        {
            container.Name = std::format("{}-{}-1", spec.ProjectName, container.ServiceName);
        }

        ValidateName(container.Name.c_str(), WSLC_MAX_CONTAINER_NAME_LENGTH);
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            wsl::shared::Localization::MessageWslcComposeDuplicateContainerName(container.Name.c_str()),
            !containerNames.insert(container.Name).second);
    }

    return spec;
}

std::string ComposeNormalizer::DefaultNetworkName(std::string_view projectName)
{
    return std::format("{}_default", projectName);
}

void ComposeNormalizer::ValidateSelection(const ComposeProjectSelection& selection)
{
    THROW_HR_IF(E_NOTIMPL, !selection.Profiles.empty() || !selection.Services.empty() || selection.IncludeDependencies);
}

std::string ComposeNormalizer::ValidateProjectKey(std::string_view projectKey)
{
    THROW_HR_IF(E_INVALIDARG, !IsValidProjectKey(projectKey));
    return std::string{projectKey};
}

bool ComposeNormalizer::IsValidProjectKey(std::string_view projectKey) noexcept
{
    if (projectKey.empty() || projectKey.size() > WSLC_MAX_COMPOSE_PROJECT_NAME_LENGTH ||
        !std::isalnum(static_cast<unsigned char>(projectKey.front())))
    {
        return false;
    }

    return std::ranges::all_of(projectKey, [](unsigned char value) {
        return std::isdigit(value) || std::islower(value) || value == '-' || value == '_';
    });
}

} // namespace wsl::windows::service::wslc
