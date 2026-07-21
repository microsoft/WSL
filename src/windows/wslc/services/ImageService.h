/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageService.h

Abstract:

    This file contains the ImageService definition

--*/
#pragma once

#include "SessionModel.h"
#include "ImageModel.h"
#include "Reporter.h"
#include <map>
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {

struct BuildSecret
{
    std::wstring Id;         // value for docker's --secret id= field
    std::vector<BYTE> Value; // raw secret bytes (may contain NULs); materialized into a VM tmpfs file server-side
};

// Parsed docker-style --output spec (buildx exporter). Type/Dest are the resolved exporter type and
// destination; any remaining key=value attributes (name, push, compression, ...) are carried verbatim.
struct BuildOutput
{
    std::wstring Type;                               // resolved exporter type (e.g. L"local", L"tar", ...)
    std::wstring Dest;                               // destination path; L"-" means stdout; empty when not applicable
    std::map<std::wstring, std::wstring> Attributes; // remaining key=value attributes
};

class ImageService
{
public:
    static void Build(
        wsl::windows::wslc::models::Session& session,
        const std::wstring& contextPath,
        const std::vector<std::wstring>& tags,
        const std::vector<std::wstring>& buildArgs,
        const std::vector<std::wstring>& labels,
        const std::vector<BuildSecret>& secrets,
        const std::wstring& dockerfilePath,
        const std::wstring& target,
        const std::wstring& output,
        WSLCBuildImageFlags flags,
        IProgressCallback* callback,
        HANDLE cancelEvent = nullptr);

    static std::vector<wsl::windows::wslc::models::ImageInformation> List(
        wsl::windows::wslc::models::Session& session, const std::vector<std::pair<std::string, std::string>>& filters = {});
    static void Load(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::wstring& input, IImageLoadCallback* callback = nullptr);
    static std::string Import(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::wstring& input, const std::string& imageName);
    static void Delete(wsl::windows::wslc::models::Session& session, const std::string& image, bool force, bool noPrune);
    static wsl::windows::common::wslc_schema::InspectImage Inspect(wsl::windows::wslc::models::Session& session, const std::string& image);
    static void Pull(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    static void Push(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    static void Save(wsl::windows::wslc::models::Session& session, const std::vector<std::string>& images, const std::wstring& output, HANDLE cancelEvent = nullptr);
    static void Save(wsl::windows::wslc::models::Session& session, const std::vector<std::string>& images, HANDLE outputHandle, HANDLE cancelEvent = nullptr);
    static void Tag(wsl::windows::wslc::models::Session& session, const std::string& sourceImage, const std::string& targetImage);
    static wsl::windows::wslc::models::PruneImagesResult Prune(
        wsl::windows::wslc::models::Session& session, bool all, const std::vector<std::pair<std::string, std::string>>& filters = {});
};
} // namespace wsl::windows::wslc::services
