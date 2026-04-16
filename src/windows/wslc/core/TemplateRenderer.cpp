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
int TryRenderGoTemplate(const char* templateStr, const char* jsonData, char** output);
void FreeGoString(char* ptr);
}

namespace wsl::windows::wslc::core {

using namespace wsl::shared::string;

TemplateRenderer::RenderResult TemplateRenderer::TryRender(const std::string& templateStr, const std::string& jsonData, std::wstring& output)
{
    try
    {
        char* rawOutput = nullptr;
        auto success = TryRenderGoTemplate(templateStr.c_str(), jsonData.c_str(), &rawOutput);

        std::string result(rawOutput ? rawOutput : "");
        FreeGoString(rawOutput);

        output = MultiByteToWide(result);
        return static_cast<RenderResult>(success);
    }
    catch (const std::exception& ex)
    {
        output = MultiByteToWide(ex.what());
        return RenderResult::Fail_Unknown;
    }
}

void TemplateRenderer::Render(const std::string& templateStr, const std::string& jsonData, std::wstring& output)
{
    switch (TryRender(templateStr, jsonData, output))
    {
    case RenderResult::Success:
        return;
    case RenderResult::Fail_NullPointer:
        THROW_HR(E_POINTER);
    case RenderResult::Fail_ParseJSON:
    case RenderResult::Fail_ParseTemplate:
    case RenderResult::Fail_ExecuteTemplate:
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, output);
    case RenderResult::Fail_Unknown:
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

} // namespace wsl::windows::wslc::core
