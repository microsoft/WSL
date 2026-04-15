/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TemplateRenderer.h

Abstract:

    This file contains the interface for rendering Go templates with JSON data.

--*/

#pragma once

#include <string>
#include <memory>

namespace wsl::windows::wslc::core {

struct TemplateRenderer
{
    // Renders a Go template with the provided JSON data.
    // Returns the rendered output as a wide string.
    // Throws an exception if template rendering fails.
    static std::wstring Render(const std::string& templateStr, const std::string& jsonData);
};

} // namespace wsl::windows::wslc::core