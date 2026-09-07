/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.h

Abstract:

    Defines minimal compose CLI operations.

--*/

#pragma once

#include "ComposeModel.h"
#include "SessionModel.h"
#include "Terminal.h"
#include <filesystem>
#include <variant>

namespace wsl::windows::wslc::services {

struct ComposeProjectReference
{
    explicit ComposeProjectReference(std::filesystem::path path) : Value(std::move(path))
    {
    }

    explicit ComposeProjectReference(std::string projectKey) : Value(std::move(projectKey))
    {
    }

    std::variant<std::filesystem::path, std::string> Value;
};

struct ComposeService
{
    static std::vector<models::ComposeProjectInformation> List(models::Session& session, bool all);
    static void Create(Terminal& terminal, models::Session& session, const std::wstring& path, HANDLE cancelEvent);
    static int Up(Terminal& terminal, models::Session& session, const std::wstring& path, HANDLE cancelEvent, HANDLE forceCancelEvent);
    static void Start(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent);
    static int Attach(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent);
    static void Stop(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, ULONG timeout, HANDLE cancelEvent);
    static void Remove(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent);
};

} // namespace wsl::windows::wslc::services
