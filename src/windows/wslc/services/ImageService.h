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
#include <wslc_schema.h>

namespace wsl::windows::wslc::services {
class ImageService
{
public:
    static void Build(
        wsl::windows::wslc::models::Session& session,
        const std::wstring& contextPath,
        const std::vector<std::wstring>& tags,
        const std::vector<std::wstring>& buildArgs,
        const std::wstring& dockerfilePath,
        bool verbose,
        IProgressCallback* callback,
        HANDLE cancelEvent = nullptr);

    static std::vector<wsl::windows::wslc::models::ImageInformation> List(wsl::windows::wslc::models::Session& session);
    static void Load(wsl::windows::wslc::models::Session& session, const std::wstring& input);
    static void Delete(wsl::windows::wslc::models::Session& session, const std::string& image, bool force, bool noPrune);
    static wsl::windows::common::wslc_schema::InspectImage Inspect(wsl::windows::wslc::models::Session& session, const std::string& image);
    static void Pull(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    static void Save(wsl::windows::wslc::models::Session& session, const std::string& image, const std::wstring& output, HANDLE cancelEvent = nullptr);
    static void Tag(wsl::windows::wslc::models::Session& session, const std::string& sourceImage, const std::string& targetImage);
    void Push();
    void Prune();
};
} // namespace wsl::windows::wslc::services
