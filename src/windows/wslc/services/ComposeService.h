/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.h

Abstract:

    Defines minimal compose CLI operations.

--*/

#pragma once

#include "ComposeModel.h"
#include "ContainerModel.h"
#include "SessionModel.h"
#include "Terminal.h"
#include <filesystem>
#include <string>
#include <variant>

namespace wsl::windows::wslc::services {

using namespace wsl::windows::wslc::cli;

using ComposeProjectReference = std::variant<std::filesystem::path, std::string>;

struct ComposeService
{
    static std::wstring FormatProjectStatus(const WSLCComposeProjectSummary& project, models::FormatType format);
    static std::vector<models::ComposeProjectInformation> List(models::Session& session, bool all, models::FormatType format = models::FormatType::Json);
    static void Create(Terminal& terminal, models::Session& session, const std::wstring& path, HANDLE cancelEvent);
    static int Up(Terminal& terminal, models::Session& session, const std::wstring& path, HANDLE cancelEvent);
    static void Start(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent);
    static int Attach(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent);
    static void Stop(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, ULONG timeout, HANDLE cancelEvent);
    static void Remove(Terminal& terminal, models::Session& session, const ComposeProjectReference& project, HANDLE cancelEvent);
};

} // namespace wsl::windows::wslc::services
