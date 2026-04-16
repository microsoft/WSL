/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    GoTemplateRenderer.cpp

Abstract:

    Implementation of the Go template renderer.

--*/

#include "TemplateRenderer.h"
#include <string>

// Use the cgo-generated header from the Go template renderer build
#include "render.h"

namespace wsl::windows::wslc::core {

using namespace wsl::shared::string;

bool TemplateRenderer::TryRender(const std::string& templateStr, const std::string& jsonData, std::wstring& output)
{
    char* rawOutput = nullptr;
    auto success = TryRenderGoTemplate(const_cast<char*>(templateStr.c_str()), const_cast<char*>(jsonData.c_str()), &rawOutput);

    std::string result(rawOutput ? rawOutput : "");
    FreeGoString(rawOutput);

    output = MultiByteToWide(result);
    return success != 0;
}

} // namespace wsl::windows::wslc::core
