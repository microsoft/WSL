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
    ULONGLONG Size{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageInformation, Repository, Tag, Id, Created, Size);
};
} // namespace wsl::windows::wslc::models
