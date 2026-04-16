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

        // Catch-all for any unknown errors
        Fail_Unknown = -1,
    };

    // Renders a Go template with the provided JSON data.
    // Returns true on success with the rendered output, false on failure with an error message.
    static RenderResult TryRender(const std::string& templateStr, const std::string& jsonData, std::wstring& output);

    static void Render(const std::string& templateStr, const std::string& jsonData, std::wstring& output);
};

} // namespace wsl::windows::wslc::core