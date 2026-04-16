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
    // Renders a Go template with the provided JSON data.
    // Returns true on success with the rendered output, false on failure with an error message.
    static bool TryRender(const std::string& templateStr, const std::string& jsonData, std::wstring& output);
};

} // namespace wsl::windows::wslc::core