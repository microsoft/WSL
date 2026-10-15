// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <filesystem>
#include <istream>
#include <optional>
#include <string>

namespace wsl::windows::common::elfcore {

// What the guest kernel recorded about a crashed process.
struct CoreDetails
{
    // The process architecture, from e_machine: "x86_64" or "aarch64".
    std::string Architecture;

    // The signal that terminated the process, from NT_PRSTATUS.
    int Signal{};

    // si_code from NT_SIGINFO. Empty when the core has no NT_SIGINFO note.
    std::optional<int> SignalCode;

    // The path the process was executed as, from AT_EXECFN. Empty when the string is not in the core.
    std::string ExecutablePath;

    // Lowercase hex GNU build ID of the main executable. Empty when the executable has none or its
    // ELF headers are not in the core.
    std::string ExecutableBuildId;

    // Where the crashing thread was executing.
    ULONGLONG ProgramCounter{};

    // The file mapped at ProgramCounter, as recorded by the kernel. Empty when the program counter
    // is not inside a file-backed mapping, for instance after a jump through a corrupt pointer.
    std::string ModulePath;

    // The address the module is loaded at and the program counter relative to it. The offset is
    // only meaningful when ModulePath is set.
    ULONGLONG ModuleBase{};
    ULONGLONG ModuleOffset{};

    // Lowercase hex GNU build ID of the module at ProgramCounter, empty when unknown.
    std::string ModuleBuildId;
};

// Reads the crash details of a 64-bit little-endian x86_64 or aarch64 Linux core. The faulting
// thread's program counter is resolved against the file mappings the kernel recorded alongside
// it, and build IDs are read from the ELF header pages the kernel dumps for each mapped binary.
//
// Cores arrive from the utility VM and are treated as untrusted. Malformed or unsupported input
// throws HRESULT_FROM_WIN32(ERROR_BAD_FORMAT). Details that are missing from an otherwise valid
// core are left empty.
CoreDetails ReadCoreDetails(_In_ std::istream& Core);

CoreDetails ReadCoreDetails(_In_ const std::filesystem::path& Core);

} // namespace wsl::windows::common::elfcore
