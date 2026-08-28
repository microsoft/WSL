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
#include "Terminal.h"
#include <map>
#include <optional>
#include <vector>
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {

struct BuildSecret
{
    std::wstring Id; // value for docker's --secret id= field
    // For file (src=) secrets: the resolved absolute host path. The service mounts the file's parent
    // directory into the build VM read-only and references the file in place, so the bytes are never
    // copied off their original (possibly EFS-encrypted) location. Empty for env/in-memory secrets.
    std::wstring SourcePath;
    // For env/in-memory secrets: the raw secret bytes (may contain NULs), materialized into a host-side
    // file mounted read-only into the VM during the build. Empty for file secrets.
    std::vector<BYTE> Value;
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
        const std::optional<BuildOutput>& output,
        const std::optional<std::wstring>& iidFilePath,
        WSLCBuildImageFlags flags,
        IProgressCallback* callback,
        HANDLE cancelEvent = nullptr);

    // Container counts are only gathered when requested: it costs an extra query, and the service
    // computes it alongside the image list so the two are consistent.
    static std::vector<wsl::windows::wslc::models::ImageInformation> List(
        wsl::windows::wslc::models::Session& session,
        const std::vector<std::pair<std::string, std::string>>& filters = {},
        bool containerCounts = false);
    static void Load(Terminal& terminal, wsl::windows::wslc::models::Session& session, const std::wstring& input, IImageLoadCallback* callback = nullptr);
    static std::string Import(Terminal& terminal, wsl::windows::wslc::models::Session& session, const std::wstring& input, const std::string& imageName);
    static std::vector<wsl::windows::wslc::models::DeletedImageEntry> Delete(
        wsl::windows::wslc::models::Session& session, const std::string& image, bool force, bool noPrune);
    static wsl::windows::common::wslc_schema::InspectImage Inspect(wsl::windows::wslc::models::Session& session, const std::string& image);
    static void Pull(Terminal& terminal, wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    static void Push(Terminal& terminal, wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    static void Save(wsl::windows::wslc::models::Session& session, const std::vector<std::string>& images, const std::wstring& output, HANDLE cancelEvent = nullptr);
    static void Save(wsl::windows::wslc::models::Session& session, const std::vector<std::string>& images, HANDLE outputHandle, HANDLE cancelEvent = nullptr);
    static void Tag(wsl::windows::wslc::models::Session& session, const std::string& sourceImage, const std::string& targetImage);
    static wsl::windows::wslc::models::PruneImagesResult Prune(
        wsl::windows::wslc::models::Session& session, bool all, const std::vector<std::pair<std::string, std::string>>& filters = {});
};
} // namespace wsl::windows::wslc::services
