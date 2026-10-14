// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <filesystem>
#include <istream>
#include <string>

namespace wsl::windows::common::elfcore {

// Where the crashing thread was executing when the guest kernel wrote the core.
struct FaultLocation
{
    ULONGLONG ProgramCounter{};

    // The file mapped at ProgramCounter, as recorded by the kernel. Empty when the program counter
    // is not inside a file-backed mapping, for instance after a jump through a corrupt pointer.
    std::string ModulePath;

    // The address the module is loaded at and the program counter relative to it. The offset is
    // only meaningful when ModulePath is set.
    ULONGLONG ModuleBase{};
    ULONGLONG ModuleOffset{};
};

// Reads the crashing thread's program counter from a 64-bit little-endian x86_64 or aarch64 Linux
// core and resolves it against the file mappings the kernel recorded alongside it.
//
// Cores arrive from the utility VM and are treated as untrusted. Malformed or unsupported input
// throws HRESULT_FROM_WIN32(ERROR_BAD_FORMAT).
FaultLocation ReadFaultLocation(_In_ std::istream& Core);

FaultLocation ReadFaultLocation(_In_ const std::filesystem::path& Core);

} // namespace wsl::windows::common::elfcore
