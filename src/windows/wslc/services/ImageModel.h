/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageModel.h

Abstract:

    This file contains the ImageModel definition.

--*/
#pragma once

namespace wsl::windows::wslc::models {
struct ImageInformation
{
    std::optional<std::string> Repository;
    std::optional<std::string> Tag;
    std::string Id;
    LONGLONG Created{};
    int64_t Size{};
    // Number of containers created from the image, or -1 when the count wasn't requested.
    LONGLONG Containers{-1};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageInformation, Repository, Tag, Id, Created, Size);
};

// The shape emitted by "image list --format json". Every value is reported as a string, and
// "<none>" is used rather than null for missing repository, tag, and digest data, so this is kept
// separate from ImageInformation, which mirrors the service's native types.
struct ImageOutputInformation
{
    std::string Containers;
    std::string CreatedAt;
    std::string CreatedSince;
    std::string Digest;
    std::string ID;
    std::string Repository;
    std::string SharedSize;
    std::string Size;
    std::string Tag;
    std::string UniqueSize;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ImageOutputInformation, Containers, CreatedAt, CreatedSince, Digest, ID, Repository, SharedSize, Size, Tag, UniqueSize);
};

inline constexpr std::string_view c_none = "<none>";

struct DeletedImageEntry
{
    std::string Image;
    bool Deleted{};
};

struct PruneImagesResult
{
    std::vector<std::string> DeletedImages;
    std::vector<std::string> UntaggedImages;
    ULONGLONG SpaceReclaimed{};
};
} // namespace wsl::windows::wslc::models
