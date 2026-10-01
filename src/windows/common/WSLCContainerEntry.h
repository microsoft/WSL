// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wil/resource.h>
#include "wslc.h"

namespace wsl::windows::common::wslc {

// IWSLCSession::ListContainers allocates a string for every unbounded field on an entry, so each
// element owns memory that releasing the array alone would miss. Safe to call on a partially
// populated entry because unset fields are null.
inline void FreeContainerEntryStrings(_Inout_ WSLCContainerEntry* Entry)
{
    CoTaskMemFree(Entry->Command);
    CoTaskMemFree(Entry->Status);
    CoTaskMemFree(Entry->Labels);
    CoTaskMemFree(Entry->Networks);
    CoTaskMemFree(Entry->Mounts);
}

// Owns both the entry array and the per-entry strings. Callers of ListContainers should use this
// rather than a plain cotaskmem array so the strings cannot be leaked. The array still holds raw
// entries, so it can be passed to the interface as-is.
using unique_container_entry = wil::unique_struct<WSLCContainerEntry, decltype(&FreeContainerEntryStrings), FreeContainerEntryStrings>;

using unique_container_entry_array = wil::unique_cotaskmem_array_ptr<unique_container_entry>;

} // namespace wsl::windows::common::wslc
