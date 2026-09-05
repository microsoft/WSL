// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeNormalizer.h"

namespace wsl::windows::service::wslc {

namespace {

    constexpr ULONG c_maxComposeDocumentSize = 16 * 1024 * 1024;

    std::string NormalizeProjectName(std::string_view Name)
    {
        std::string result{Name};
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

ComposeSpec ComposeNormalizer::Normalize(const ComposeDocuments& Documents, const ComposeProjectSelection& Selection)
{
    THROW_HR_IF(E_INVALIDARG, Documents.SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);
    THROW_HR_IF(E_INVALIDARG, Documents.WorkingDirectory.empty());
    THROW_HR_IF(E_INVALIDARG, Documents.ProjectDirectory.empty());
    THROW_HR_IF(E_INVALIDARG, Documents.Documents.size() != 1);
    ValidateSelection(Selection);

    const auto& document = Documents.Documents.front();
    THROW_HR_IF(E_INVALIDARG, document.SourcePath.empty());
    THROW_HR_IF(E_INVALIDARG, document.BaseDirectory.empty());
    THROW_HR_IF(E_INVALIDARG, document.Content.empty() || document.Content.size() > c_maxComposeDocumentSize);

    std::string content(reinterpret_cast<const char*>(document.Content.data()), document.Content.size());
    THROW_HR_IF(E_INVALIDARG, content.find('\0') != std::string::npos);

    auto spec = ComposeSpec::Parse(document.SourcePath, content);

    std::string projectName;
    if (Documents.ExplicitProjectName.has_value())
    {
        projectName = NormalizeProjectName(*Documents.ExplicitProjectName);
    }
    else
    {
        auto projectDirectory = Documents.ProjectDirectory.lexically_normal();
        if (projectDirectory.filename().empty())
        {
            projectDirectory = projectDirectory.parent_path();
        }

        projectName = NormalizeProjectName(wsl::shared::string::WideToMultiByte(projectDirectory.filename().wstring()));
    }

    spec.ProjectName = std::move(projectName);
    for (auto& container : spec.Containers)
    {
        if (container.Name.empty())
        {
            container.Name = std::format("{}-{}-1", spec.ProjectName, container.ServiceName);
        }
    }

    return spec;
}

void ComposeNormalizer::ValidateSelection(const ComposeProjectSelection& Selection)
{
    THROW_HR_IF(E_NOTIMPL, !Selection.Profiles.empty() || !Selection.Services.empty() || Selection.IncludeDependencies);
}

std::string ComposeNormalizer::ValidateProjectKey(std::string_view ProjectKey)
{
    THROW_HR_IF(E_INVALIDARG, !IsValidProjectKey(ProjectKey));
    return std::string{ProjectKey};
}

bool ComposeNormalizer::IsValidProjectKey(std::string_view ProjectKey) noexcept
{
    if (ProjectKey.empty() || ProjectKey.size() > WSLC_MAX_COMPOSE_PROJECT_NAME_LENGTH ||
        !std::isalnum(static_cast<unsigned char>(ProjectKey.front())))
    {
        return false;
    }

    return std::ranges::all_of(ProjectKey, [](unsigned char value) {
        return std::isdigit(value) || std::islower(value) || value == '-' || value == '_';
    });
}

} // namespace wsl::windows::service::wslc
