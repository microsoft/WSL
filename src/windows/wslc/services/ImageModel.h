/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageModel.h

Abstract:

    This file contains the ImageModel definition.

--*/
#pragma once

// 1000*1000 instead of 1024*1024 to be consistent with Docker CLI's definition of megabyte (MB).
#define WSLC_IMAGE_1MB (1000 * 1000)

namespace wsl::windows::wslc::models {
struct ImageInformation
{
    std::optional<std::string> Repository;
    std::optional<std::string> Tag;
    std::string Id;
    LONGLONG Created{};
    ULONGLONG Size{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageInformation, Repository, Tag, Id, Created, Size);
};

struct PruneImagesResult
{
    std::vector<std::string> DeletedImages;
    std::vector<std::string> UntaggedImages;
    ULONGLONG SpaceReclaimed{};
};
} // namespace wsl::windows::wslc::models
