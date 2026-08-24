// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wil/resource.h>
#include "wslc.h"

namespace wsl::windows::common::wslc {

// IWSLCSession::ListContainers allocates a string for every unbounded field on an entry, so each
// element owns memory that releasing the array alone would miss. Safe to call on a partially
// populated entry because unset fields are null.
inline void FreeContainerEntryStrings(_Inout_ WSLCContainerEntry& Entry)
{
    CoTaskMemFree(Entry.Command);
    CoTaskMemFree(Entry.Status);
    CoTaskMemFree(Entry.Labels);
    CoTaskMemFree(Entry.Networks);
    CoTaskMemFree(Entry.Mounts);
}

// Owns both the entry array and the per-entry strings. Callers of ListContainers should use this
// rather than a plain cotaskmem array so the strings cannot be leaked. WIL requires the element
// deleter to be a default constructible callable, so the cleanup function is adapted by a lambda.
inline constexpr auto c_containerEntryDeleter = [](WSLCContainerEntry& Entry) { FreeContainerEntryStrings(Entry); };

using unique_container_entry_array =
    wil::unique_any_array_ptr<WSLCContainerEntry, wil::cotaskmem_deleter, decltype(c_containerEntryDeleter)>;

} // namespace wsl::windows::common::wslc
