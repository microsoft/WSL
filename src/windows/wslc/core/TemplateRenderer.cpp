/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    GoTemplateRenderer.cpp

Abstract:

    Implementation of the Go template renderer.

--*/

#include "TemplateRenderer.h"
#include <wil/result_macros.h>
#include <windows.h>
#include <string>
#include <stdexcept>

// Use the cgo-generated header from the Go template renderer build
#include "render.h"

namespace wsl::windows::wslc::core {

using namespace wsl::shared::string;

std::wstring TemplateRenderer::Render(const std::string& templateStr, const std::string& jsonData)
{
    // Call the Go template renderer
    char* result = RenderGoTemplate(const_cast<char*>(templateStr.c_str()), const_cast<char*>(jsonData.c_str()));

    if (result == nullptr)
    {
        throw std::runtime_error("error: Go template renderer returned null");
    }

    // Check if result is an error message (starts with "error:")
    std::string resultStr(result);

    // Free the memory allocated by Go
    FreeMemory(result);

    // If it's an error, throw an exception
    if (resultStr.starts_with("error:"))
    {
        throw std::runtime_error(resultStr);
    }

    return MultiByteToWide(resultStr);
}

} // namespace wsl::windows::wslc::core
