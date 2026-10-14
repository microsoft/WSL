// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "CrashReporting.h"

#include <sstream>

using wsl::windows::common::crashreporting::BuildReportParameters;
using wsl::windows::common::crashreporting::c_parameterCount;
using wsl::windows::common::crashreporting::GetExecutableName;
using wsl::windows::common::crashreporting::LinuxCrashInfo;
using wsl::windows::common::elfcore::FaultLocation;
using wsl::windows::common::elfcore::ReadFaultLocation;

namespace CrashReportingUnitTests {

namespace {

    constexpr uint16_t c_machineX86_64 = 62;
    constexpr uint16_t c_machineAarch64 = 183;
    constexpr uint16_t c_typeCore = 4;
    constexpr uint32_t c_noteProcessStatus = 1;
    constexpr uint32_t c_noteMappedFiles = 0x46494c45;
    constexpr uint64_t c_pageSize = 0x1000;

    // sizeof(struct elf_prstatus) and the offset of the program counter within it.
    constexpr size_t c_x86_64PrStatusSize = 336;
    constexpr size_t c_x86_64ProgramCounterOffset = 240;
    constexpr size_t c_aarch64PrStatusSize = 392;
    constexpr size_t c_aarch64ProgramCounterOffset = 368;

    struct Note
    {
        std::string Name;
        uint32_t Type;
        std::string Descriptor;
    };

    struct Mapping
    {
        uint64_t Start;
        uint64_t End;
        uint64_t FileOffsetPages;
        std::string Path;
    };

    template <typename T>
    void Append(std::string& Buffer, T Value)
    {
        Buffer.append(reinterpret_cast<const char*>(&Value), sizeof(Value));
    }

    void AlignNote(std::string& Buffer)
    {
        Buffer.resize((Buffer.size() + 3) & ~size_t{3}, '\0');
    }

    std::string PrStatus(size_t Size, size_t ProgramCounterOffset, uint64_t ProgramCounter)
    {
        std::string descriptor(Size, '\0');
        memcpy(descriptor.data() + ProgramCounterOffset, &ProgramCounter, sizeof(ProgramCounter));
        return descriptor;
    }

    std::string X86_64PrStatus(uint64_t ProgramCounter)
    {
        return PrStatus(c_x86_64PrStatusSize, c_x86_64ProgramCounterOffset, ProgramCounter);
    }

    std::string MappedFiles(const std::vector<Mapping>& Mappings)
    {
        std::string descriptor;
        Append(descriptor, static_cast<uint64_t>(Mappings.size()));
        Append(descriptor, c_pageSize);
        for (const auto& mapping : Mappings)
        {
            Append(descriptor, mapping.Start);
            Append(descriptor, mapping.End);
            Append(descriptor, mapping.FileOffsetPages);
        }

        for (const auto& mapping : Mappings)
        {
            descriptor.append(mapping.Path);
            descriptor.push_back('\0');
        }

        return descriptor;
    }

    // Builds a minimal ELF core: the file header, a single PT_NOTE program header and its notes.
    std::string BuildCore(uint16_t Machine, const std::vector<Note>& Notes, uint16_t Type = c_typeCore)
    {
        std::string notes;
        for (const auto& note : Notes)
        {
            Append(notes, static_cast<uint32_t>(note.Name.size() + 1));
            Append(notes, static_cast<uint32_t>(note.Descriptor.size()));
            Append(notes, note.Type);
            notes.append(note.Name);
            notes.push_back('\0');
            AlignNote(notes);
            notes.append(note.Descriptor);
            AlignNote(notes);
        }

        constexpr uint64_t headerSize = 64;
        constexpr uint64_t programHeaderSize = 56;

        std::string core{'\x7f', 'E', 'L', 'F', 2, 1, 1};
        core.resize(16, '\0');
        Append(core, Type);
        Append(core, Machine);
        Append(core, uint32_t{1});
        Append(core, uint64_t{0});
        Append(core, headerSize);
        Append(core, uint64_t{0});
        Append(core, uint32_t{0});
        Append(core, static_cast<uint16_t>(headerSize));
        Append(core, static_cast<uint16_t>(programHeaderSize));
        Append(core, uint16_t{1});
        Append(core, uint16_t{0});
        Append(core, uint16_t{0});
        Append(core, uint16_t{0});

        Append(core, uint32_t{4});
        Append(core, uint32_t{0});
        Append(core, headerSize + programHeaderSize);
        Append(core, uint64_t{0});
        Append(core, uint64_t{0});
        Append(core, static_cast<uint64_t>(notes.size()));
        Append(core, uint64_t{0});
        Append(core, uint64_t{4});

        core.append(notes);
        return core;
    }

    FaultLocation Read(const std::string& Core)
    {
        std::istringstream stream(Core);
        return ReadFaultLocation(stream);
    }

    void VerifyRejected(const std::string& Core)
    {
        VERIFY_THROWS_SPECIFIC(Read(Core), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        });
    }

    const std::vector<Mapping> c_mappings{
        {0x400000, 0x401000, 0, "/usr/bin/bash"},
        {0x401000, 0x480000, 1, "/usr/bin/bash"},
        {0x7f0000000000, 0x7f0000028000, 0, "/usr/lib/x86_64-linux-gnu/libc.so.6"},
        {0x7f0000028000, 0x7f00001bd000, 0x28, "/usr/lib/x86_64-linux-gnu/libc.so.6"},
        {0x7f00001bd000, 0x7f0000215000, 0x1bd, "/usr/lib/x86_64-linux-gnu/libc.so.6"},
    };

} // namespace

class CrashReportingUnitTests
{
    WSL_TEST_CLASS(CrashReportingUnitTests)

    TEST_METHOD(ExecutableNameFromCorePatternEncoding)
    {
        // core_pattern's %E specifier encodes path separators as '!'.
        VERIFY_ARE_EQUAL(L"bash", GetExecutableName("!usr!bin!bash"));
        VERIFY_ARE_EQUAL(L"dash", GetExecutableName("!usr!bin!dash"));
    }

    TEST_METHOD(ExecutableNameFromConventionalPath)
    {
        VERIFY_ARE_EQUAL(L"bash", GetExecutableName("/usr/bin/bash"));
        VERIFY_ARE_EQUAL(L"crashdemo", GetExecutableName("crashdemo"));
    }

    TEST_METHOD(ExecutableNameFallsBackWhenEmpty)
    {
        VERIFY_ARE_EQUAL(L"unknown", GetExecutableName(""));
        VERIFY_ARE_EQUAL(L"unknown", GetExecutableName("!usr!bin!"));
    }

    TEST_METHOD(ParametersMatchRegisteredSignature)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Pid = 42;
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        // Watson rejects the report outright when the parameter count or the names do not match
        // the signature registered for WSLAppCrash.
        VERIFY_ARE_EQUAL(static_cast<size_t>(7), c_parameterCount);
        VERIFY_ARE_EQUAL(c_parameterCount, parameters.size());

        VERIFY_ARE_EQUAL(L"AppName", parameters[0].first);
        VERIFY_ARE_EQUAL(L"AppVersion", parameters[1].first);
        VERIFY_ARE_EQUAL(L"ModuleName", parameters[2].first);
        VERIFY_ARE_EQUAL(L"ModuleVersion", parameters[3].first);
        VERIFY_ARE_EQUAL(L"ModuleOffset", parameters[4].first);
        VERIFY_ARE_EQUAL(L"ExceptionCode", parameters[5].first);
        VERIFY_ARE_EQUAL(L"Flags", parameters[6].first);
    }

    TEST_METHOD(ParameterValuesDescribeTheCrash)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        VERIFY_ARE_EQUAL(L"bash", parameters[0].second);
        VERIFY_ARE_EQUAL(L"bash", parameters[2].second);
        VERIFY_ARE_EQUAL(L"0000000b", parameters[5].second);
        VERIFY_ARE_EQUAL(L"0", parameters[6].second);

        // Every parameter has to carry a value, an empty one changes the signature Watson sees.
        for (const auto& parameter : parameters)
        {
            VERIFY_IS_FALSE(parameter.first.empty());
            VERIFY_IS_FALSE(parameter.second.empty());
        }
    }

    TEST_METHOD(AppNameAttributesTheCrashToTheCallingApplication)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;
        info.AppName = L"contoso.exe";

        const auto parameters = BuildReportParameters(info);

        // The report buckets against the calling application while the module still names the
        // Linux binary that faulted.
        VERIFY_ARE_EQUAL(L"contoso.exe", parameters[0].second);
        VERIFY_ARE_EQUAL(L"bash", parameters[2].second);
    }

    TEST_METHOD(AppNameFallsBackToTheCrashingExecutable)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        VERIFY_ARE_EQUAL(parameters[2].second, parameters[0].second);
    }

    TEST_METHOD(SignalIsFormattedAsExceptionCode)
    {
        LinuxCrashInfo info{};
        info.Executable = "crashdemo";

        info.Signal = 6;
        VERIFY_ARE_EQUAL(L"00000006", BuildReportParameters(info)[5].second);

        info.Signal = 8;
        VERIFY_ARE_EQUAL(L"00000008", BuildReportParameters(info)[5].second);
    }

    TEST_METHOD(CoreResolvesFaultingModuleOnX86_64)
    {
        // A note under a different owner with a colliding type has to be ignored.
        const auto core = BuildCore(
            c_machineX86_64,
            {{"LINUX", c_noteProcessStatus, X86_64PrStatus(0x1111)},
             {"CORE", c_noteProcessStatus, X86_64PrStatus(0x7f0000032345)},
             {"CORE", c_noteMappedFiles, MappedFiles(c_mappings)}});

        const auto fault = Read(core);

        // The program counter sits in libc's second segment, so the offset is relative to the
        // first segment's load address.
        VERIFY_ARE_EQUAL(0x7f0000032345ull, fault.ProgramCounter);
        VERIFY_ARE_EQUAL(std::string("/usr/lib/x86_64-linux-gnu/libc.so.6"), fault.ModulePath);
        VERIFY_ARE_EQUAL(0x7f0000000000ull, fault.ModuleBase);
        VERIFY_ARE_EQUAL(0x32345ull, fault.ModuleOffset);
    }

    TEST_METHOD(CoreResolvesFaultingModuleOnAarch64)
    {
        const auto core = BuildCore(
            c_machineAarch64,
            {{"CORE", c_noteProcessStatus, PrStatus(c_aarch64PrStatusSize, c_aarch64ProgramCounterOffset, 0x401234)},
             {"CORE", c_noteMappedFiles, MappedFiles(c_mappings)}});

        const auto fault = Read(core);

        VERIFY_ARE_EQUAL(std::string("/usr/bin/bash"), fault.ModulePath);
        VERIFY_ARE_EQUAL(0x400000ull, fault.ModuleBase);
        VERIFY_ARE_EQUAL(0x1234ull, fault.ModuleOffset);
    }

    TEST_METHOD(CoreUsesTheFirstThread)
    {
        // The kernel writes the thread that triggered the dump first.
        const auto core = BuildCore(
            c_machineX86_64,
            {{"CORE", c_noteProcessStatus, X86_64PrStatus(0x401000)},
             {"CORE", c_noteProcessStatus, X86_64PrStatus(0x7f0000000010)},
             {"CORE", c_noteMappedFiles, MappedFiles(c_mappings)}});

        const auto fault = Read(core);

        VERIFY_ARE_EQUAL(0x401000ull, fault.ProgramCounter);
        VERIFY_ARE_EQUAL(std::string("/usr/bin/bash"), fault.ModulePath);
        VERIFY_ARE_EQUAL(0x1000ull, fault.ModuleOffset);
    }

    TEST_METHOD(CoreProgramCounterOutsideAnyModule)
    {
        const auto core = BuildCore(
            c_machineX86_64, {{"CORE", c_noteProcessStatus, X86_64PrStatus(0xdead)}, {"CORE", c_noteMappedFiles, MappedFiles(c_mappings)}});

        const auto fault = Read(core);

        VERIFY_ARE_EQUAL(0xdeadull, fault.ProgramCounter);
        VERIFY_IS_TRUE(fault.ModulePath.empty());
    }

    TEST_METHOD(MalformedCoresAreRejected)
    {
        const std::vector<Note> notes{
            {"CORE", c_noteProcessStatus, X86_64PrStatus(0x401000)}, {"CORE", c_noteMappedFiles, MappedFiles(c_mappings)}};

        VerifyRejected("not an elf file at all, but long enough to hold an ELF header for the check");
        VerifyRejected(BuildCore(c_machineX86_64, notes, 2));
        VerifyRejected(BuildCore(3, notes));

        // No NT_PRSTATUS means there is no faulting thread to report.
        VerifyRejected(BuildCore(c_machineX86_64, {{"CORE", c_noteMappedFiles, MappedFiles(c_mappings)}}));

        // A PT_NOTE segment that runs past the end of the file.
        auto truncated = BuildCore(c_machineX86_64, notes);
        truncated.resize(truncated.size() - 8);
        VerifyRejected(truncated);

        // An NT_FILE count larger than its descriptor can hold.
        auto mappedFiles = MappedFiles(c_mappings);
        const uint64_t count = 0x1000000;
        memcpy(mappedFiles.data(), &count, sizeof(count));
        VerifyRejected(BuildCore(
            c_machineX86_64, {{"CORE", c_noteProcessStatus, X86_64PrStatus(0x401000)}, {"CORE", c_noteMappedFiles, mappedFiles}}));

        // A program counter descriptor too small to hold the register set.
        VerifyRejected(BuildCore(c_machineX86_64, {{"CORE", c_noteProcessStatus, std::string(64, '\0')}}));
    }

    TEST_METHOD(ParametersNameTheFaultingModule)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;

        FaultLocation fault{};
        fault.ProgramCounter = 0x7f0000032345;
        fault.ModulePath = "/usr/lib/x86_64-linux-gnu/libc.so.6";
        fault.ModuleBase = 0x7f0000000000;
        fault.ModuleOffset = 0x32345;

        auto parameters = BuildReportParameters(info, fault);
        VERIFY_ARE_EQUAL(L"bash", parameters[0].second);
        VERIFY_ARE_EQUAL(L"libc.so.6", parameters[2].second);
        VERIFY_ARE_EQUAL(L"0000000000032345", parameters[4].second);

        // Without a module the absolute address is the only meaningful offset.
        fault.ModulePath.clear();
        parameters = BuildReportParameters(info, fault);
        VERIFY_ARE_EQUAL(L"unknown", parameters[2].second);
        VERIFY_ARE_EQUAL(L"00007f0000032345", parameters[4].second);

        // Without a parsed core the report falls back to the crashing executable.
        parameters = BuildReportParameters(info);
        VERIFY_ARE_EQUAL(L"bash", parameters[2].second);
        VERIFY_ARE_EQUAL(L"0000000000000000", parameters[4].second);
    }
};
} // namespace CrashReportingUnitTests
