/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TemplateRenderer.h

Abstract:

    This file contains the interface for rendering Go templates with JSON data.

--*/

#pragma once

#include <string>

namespace wsl::windows::wslc::core {

struct TemplateRenderer
{
    enum class RenderResult
    {
        Success = 0,
        Fail_NullPointer = 1,
        Fail_ParseJSON = 2,
        Fail_ParseTemplate = 3,
        Fail_ExecuteTemplate = 4,

        // All other failures
        Fail_Unknown = -1,
    };

    static RenderResult TryRender(const std::string& templateStr, const std::string& jsonData, std::wstring& output);
    static void Render(const std::string& templateStr, const std::string& jsonData, std::wstring& output);
};

} // namespace wsl::windows::wslc::core