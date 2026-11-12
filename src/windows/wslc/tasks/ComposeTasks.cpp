// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ArgumentConvertedTypes.h"
#include "ComposeService.h"
#include "ComposeTasks.h"

using namespace wsl::windows::wslc::argument;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;
using namespace wsl::shared;

namespace wsl::windows::wslc::task {

namespace {

    constexpr std::array c_defaultFileNames{L"compose.yaml", L"compose.yml", L"docker-compose.yml", L"docker-compose.yaml"};
    constexpr std::array c_defaultOverrideFileNames{
        L"compose.override.yml", L"compose.override.yaml", L"docker-compose.override.yml", L"docker-compose.override.yaml"};

    template <size_t N>
    std::optional<std::filesystem::path> FindComposeFile(const std::filesystem::path& directory, const std::array<const wchar_t*, N>& names)
    {
        for (const auto* name : names)
        {
            auto candidate = directory / name;
            std::error_code error;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
        }

        return std::nullopt;
    }

    // Searches the directory and each of its parents for a compose file and its optional override.
    std::vector<std::filesystem::path> DiscoverComposeFiles(const std::filesystem::path& startDirectory)
    {
        auto directory = startDirectory;
        while (true)
        {
            if (auto file = FindComposeFile(directory, c_defaultFileNames))
            {
                std::vector<std::filesystem::path> files{std::move(*file)};
                if (auto overrideFile = FindComposeFile(directory, c_defaultOverrideFileNames))
                {
                    files.push_back(std::move(*overrideFile));
                }

                return files;
            }

            auto parent = directory.parent_path();
            THROW_HR_WITH_USER_ERROR_IF(
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), Localization::WSLCCLI_ComposeFileNotFoundError(startDirectory.wstring()), parent == directory);

            directory = std::move(parent);
        }
    }

    std::filesystem::path ResolveDirectory(const std::filesystem::path& currentDirectory, const std::wstring& directory)
    {
        auto path = (currentDirectory / directory).lexically_normal();
        if (!path.has_filename() && path.has_relative_path())
        {
            path = path.parent_path();
        }

        return path;
    }

    bool IsLowerAlphanumeric(wchar_t character)
    {
        return (character >= L'a' && character <= L'z') || (character >= L'0' && character <= L'9');
    }

    std::optional<std::string> ProjectName(ArgMap& args)
    {
        const auto& name = args.GetValue<ArgType::ComposeProjectName>();
        if (name.empty())
        {
            return std::nullopt;
        }

        const bool valid = name.size() <= WSLC_MAX_COMPOSE_PROJECT_NAME_LENGTH && IsLowerAlphanumeric(name.front()) &&
                           std::ranges::all_of(name, [](wchar_t character) {
                               return IsLowerAlphanumeric(character) || character == L'-' || character == L'_';
                           });

        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG, Localization::WSLCCLI_ComposeProjectNameInvalidError(name, WSLC_MAX_COMPOSE_PROJECT_NAME_LENGTH), !valid);

        return string::WideToMultiByte(name);
    }

    models::ComposeProjectFiles ResolveProjectFiles(ArgMap& args, const std::filesystem::path& currentDirectory, std::optional<std::string> projectName)
    {
        std::optional<std::filesystem::path> projectDirectory;
        if (const auto& directory = args.GetValue<ArgType::ComposeProjectDirectory>(); !directory.empty())
        {
            projectDirectory = ResolveDirectory(currentDirectory, directory);
        }

        std::vector<std::filesystem::path> files;
        for (const auto& file : args.GetAllValues<ArgType::ComposeFile>())
        {
            THROW_HR_WITH_USER_ERROR_IF(E_NOTIMPL, Localization::WSLCCLI_ComposeStdinFileNotImplementedError(), file == L"-");
            files.push_back((currentDirectory / file).lexically_normal());
        }

        if (files.empty())
        {
            files = DiscoverComposeFiles(projectDirectory.value_or(currentDirectory));
        }

        models::ComposeProjectFiles project;
        project.ProjectDirectory = projectDirectory.value_or(files.front().parent_path());
        project.Files = std::move(files);
        project.ProjectName = std::move(projectName);
        return project;
    }

} // namespace

models::ComposeProject ResolveComposeProjectFromArgs(ArgMap& args, ComposeProjectSelection selection, const std::filesystem::path& currentDirectory)
{
    auto projectName = ProjectName(args);
    if (projectName.has_value())
    {
        switch (selection)
        {
        case ComposeProjectSelection::NamePreferred:
            return models::ComposeProject{std::move(*projectName)};
        case ComposeProjectSelection::NameWithoutFiles:
            if (!args.Contains(ArgType::ComposeFile))
            {
                return models::ComposeProject{std::move(*projectName)};
            }
            break;
        case ComposeProjectSelection::Files:
            break;
        }
    }

    return models::ComposeProject{ResolveProjectFiles(args, currentDirectory, std::move(projectName))};
}

void ResolveComposeProject::operator()(CLIExecutionContext& context) const
{
    context.Data.Add<Data::ComposeProject>(ResolveComposeProjectFromArgs(context.Args, m_selection, std::filesystem::current_path()));
}

void AttachCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    context.ExitCode =
        ComposeService::Attach(context.Terminal, session, context.Data.Get<Data::ComposeProject>(), context.CreateCancelEvent());
}

void CreateCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Create(context.Terminal, session, context.Data.Get<Data::ComposeProject>(), context.CreateCancelEvent());
}

void StartCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Start(context.Terminal, session, context.Data.Get<Data::ComposeProject>(), context.CreateCancelEvent());
}

void StopCompose(CLIExecutionContext& context)
{
    constexpr LONG c_defaultTimeout = 10;
    const LONG timeout = context.Args.Contains(ArgType::Time) ? context.Args.GetValue<ArgType::Time>() : c_defaultTimeout;
    THROW_HR_IF(E_INVALIDARG, timeout < 0);

    auto& session = context.Data.Get<Data::Session>();
    ComposeService::Stop(
        context.Terminal, session, context.Data.Get<Data::ComposeProject>(), static_cast<ULONG>(timeout), context.CreateCancelEvent());
}

void UpCompose(CLIExecutionContext& context)
{
    auto& session = context.Data.Get<Data::Session>();
    context.ExitCode =
        ComposeService::Up(context.Terminal, session, context.Data.Get<Data::ComposeProject>(), context.CreateCancelEvent());
}

} // namespace wsl::windows::wslc::task
