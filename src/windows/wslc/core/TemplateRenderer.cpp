/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    GoTemplateRenderer.cpp

Abstract:

    Implementation of the Go template renderer.

--*/

#include "TemplateRenderer.h"
#include <string>

// Forward-declare the Go template renderer functions (exported from render.dll).
// We declare these directly instead of including the cgo-generated render.h
// to avoid Go boilerplate types that don't compile cleanly with MSVC.
extern "C" {
int TryRenderGoTemplate(char* templateStr, char* jsonData, char** output);
void FreeGoString(char* ptr);
}

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
