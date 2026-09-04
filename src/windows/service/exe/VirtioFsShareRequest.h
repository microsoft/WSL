// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <gsl/span>

#include <functional>
#include <string>
#include <vector>

namespace wsl::windows::service {

struct VirtioFsShareResult
{
    std::wstring Tag;
    std::wstring ChildName;
    std::wstring Source;
};

using AddVirtioFsShareCallback =
    std::function<VirtioFsShareResult(bool Admin, const std::wstring& Path, const std::wstring& Options)>;
using RemountVirtioFsShareCallback =
    std::function<VirtioFsShareResult(const std::wstring& Tag, bool Admin)>;

std::vector<char> ProcessVirtioFsShareRequest(
    _In_ gsl::span<gsl::byte> Request,
    _In_ const AddVirtioFsShareCallback& AddShare,
    _In_ const RemountVirtioFsShareCallback& RemountShare);

} // namespace wsl::windows::service