// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "CrashReporting.h"

#include <sstream>

using wsl::windows::common::crashreporting::BuildReportParameters;
using wsl::windows::common::crashreporting::c_parameterCount;
using wsl::windows::common::crashreporting::GetExecutableName;
using wsl::windows::common::crashreporting::LinuxCrashInfo;
using wsl::windows::common::elfcore::CoreDetails;
using wsl::windows::common::elfcore::ReadCoreDetails;

namespace CrashReportingUnitTests {

namespace {

    constexpr uint16_t c_machineX86_64 = 62;
    constexpr uint16_t c_machineAarch64 = 183;
    constexpr uint16_t c_typeExecutable = 2;
    constexpr uint16_t c_typeSharedObject = 3;
    constexpr uint16_t c_typeCore = 4;
    constexpr uint32_t c_programHeaderLoad = 1;
    constexpr uint32_t c_programHeaderNote = 4;
    constexpr uint32_t c_noteProcessStatus = 1;
    constexpr uint32_t c_noteAuxiliaryVector = 6;
    constexpr uint32_t c_noteSignalInfo = 0x53494749;
    constexpr uint32_t c_noteMappedFiles = 0x46494c45;
    constexpr uint32_t c_noteGnuBuildId = 3;
    constexpr uint64_t c_auxProgramHeaders = 3;
    constexpr uint64_t c_auxEntry = 9;
    constexpr uint64_t c_auxExecutableName = 31;
    constexpr uint64_t c_pageSize = 0x1000;
    constexpr uint64_t c_elfHeaderSize = 64;
    constexpr uint64_t c_programHeaderSize = 56;
    constexpr size_t c_siginfoSize = 128;

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

    // Process memory captured in the core as a PT_LOAD segment.
    struct Segment
    {
        uint64_t Address;
        std::string Data;
    };

    template <typename T>
    void Append(std::string& Buffer, T Value)
    {
        Buffer.append(reinterpret_cast<const char*>(&Value), sizeof(Value));
    }

    void AlignNote(std::string& Buffer, size_t Alignment = 4)
    {
        Buffer.resize((Buffer.size() + Alignment - 1) & ~(Alignment - 1), '\0');
    }

    std::string EncodeNotes(const std::vector<Note>& Notes, size_t Alignment = 4)
    {
        std::string notes;
        for (const auto& note : Notes)
        {
            Append(notes, static_cast<uint32_t>(note.Name.size() + 1));
            Append(notes, static_cast<uint32_t>(note.Descriptor.size()));
            Append(notes, note.Type);
            notes.append(note.Name);
            notes.push_back('\0');
            AlignNote(notes, Alignment);
            notes.append(note.Descriptor);
            AlignNote(notes, Alignment);
        }

        return notes;
    }

    void AppendElfHeader(std::string& Buffer, uint16_t Type, uint16_t Machine, uint16_t ProgramHeaderCount)
    {
        Buffer.append({'\x7f', 'E', 'L', 'F', 2, 1, 1});
        Buffer.resize(Buffer.size() + 9, '\0');
        Append(Buffer, Type);
        Append(Buffer, Machine);
        Append(Buffer, uint32_t{1});
        Append(Buffer, uint64_t{0});
        Append(Buffer, c_elfHeaderSize);
        Append(Buffer, uint64_t{0});
        Append(Buffer, uint32_t{0});
        Append(Buffer, static_cast<uint16_t>(c_elfHeaderSize));
        Append(Buffer, static_cast<uint16_t>(c_programHeaderSize));
        Append(Buffer, ProgramHeaderCount);
        Append(Buffer, uint16_t{0});
        Append(Buffer, uint16_t{0});
        Append(Buffer, uint16_t{0});
    }

    void AppendProgramHeader(std::string& Buffer, uint32_t Type, uint64_t Offset, uint64_t Address, uint64_t Size, uint64_t Alignment)
    {
        Append(Buffer, Type);
        Append(Buffer, uint32_t{0});
        Append(Buffer, Offset);
        Append(Buffer, Address);
        Append(Buffer, uint64_t{0});
        Append(Buffer, Size);
        Append(Buffer, Size);
        Append(Buffer, Alignment);
    }

    // The first page of an ELF binary as the kernel dumps it: the file header, a PT_LOAD mapping
    // the start of the file at Address and a PT_NOTE holding the GNU build ID.
    std::string ElfImagePage(uint16_t Type, uint64_t Address, const std::string& BuildId, size_t NoteAlignment = 4)
    {
        constexpr uint64_t noteOffset = 0x200;

        const auto notes = EncodeNotes({{"GNU", c_noteGnuBuildId, BuildId}}, NoteAlignment);

        std::string page;
        AppendElfHeader(page, Type, c_machineX86_64, 2);
        AppendProgramHeader(page, c_programHeaderLoad, 0, Address, c_pageSize, c_pageSize);
        AppendProgramHeader(page, c_programHeaderNote, noteOffset, Address + noteOffset, notes.size(), NoteAlignment);
        page.resize(noteOffset, '\0');
        page.append(notes);
        page.resize(c_pageSize, '\0');
        return page;
    }

    std::string PrStatus(size_t Size, size_t ProgramCounterOffset, uint64_t ProgramCounter, int32_t Signal = 0)
    {
        std::string descriptor(Size, '\0');
        memcpy(descriptor.data(), &Signal, sizeof(Signal));
        memcpy(descriptor.data() + ProgramCounterOffset, &ProgramCounter, sizeof(ProgramCounter));
        return descriptor;
    }

    std::string X86_64PrStatus(uint64_t ProgramCounter, int32_t Signal = 0)
    {
        return PrStatus(c_x86_64PrStatusSize, c_x86_64ProgramCounterOffset, ProgramCounter, Signal);
    }

    std::string SigInfo(int32_t Signal, int32_t Code)
    {
        std::string descriptor;
        Append(descriptor, Signal);
        Append(descriptor, int32_t{0});
        Append(descriptor, Code);
        descriptor.resize(c_siginfoSize, '\0');
        return descriptor;
    }

    std::string AuxiliaryVector(const std::vector<std::pair<uint64_t, uint64_t>>& Entries)
    {
        std::string descriptor;
        for (const auto& [type, value] : Entries)
        {
            Append(descriptor, type);
            Append(descriptor, value);
        }

        Append(descriptor, uint64_t{0});
        Append(descriptor, uint64_t{0});
        return descriptor;
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

    // Builds a minimal ELF core: the file header, a PT_NOTE program header and its notes, then a
    // PT_LOAD program header and its contents for each segment.
    std::string BuildCore(uint16_t Machine, const std::vector<Note>& Notes, uint16_t Type = c_typeCore, const std::vector<Segment>& Segments = {})
    {
        const auto notes = EncodeNotes(Notes);

        std::string core;
        AppendElfHeader(core, Type, Machine, static_cast<uint16_t>(1 + Segments.size()));

        auto offset = c_elfHeaderSize + c_programHeaderSize * (1 + Segments.size());
        AppendProgramHeader(core, c_programHeaderNote, offset, 0, notes.size(), 4);
        offset += notes.size();

        for (const auto& segment : Segments)
        {
            AppendProgramHeader(core, c_programHeaderLoad, offset, segment.Address, segment.Data.size(), c_pageSize);
            offset += segment.Data.size();
        }

        core.append(notes);
        for (const auto& segment : Segments)
        {
            core.append(segment.Data);
        }

        return core;
    }

    CoreDetails Read(const std::string& Core)
    {
        std::istringstream stream(Core);
        return ReadCoreDetails(stream);
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

    const std::string c_bashBuildId{"\x01\x23\x45\x67\x89\xab\xcd\xef\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb", 20};
    const std::string c_libcBuildId{"\xfe\xdc\xba\x98\x76\x54\x32\x10\xff\xee\xdd\xcc\xbb\xaa\x99\x88\x77\x66\x55\x44", 20};
    constexpr uint64_t c_stackAddress = 0x7ffd00000000;
    constexpr uint64_t c_executableNameAddress = c_stackAddress + 0x10;

    // A core of bash faulting in libc, with the memory needed to read both build IDs and the
    // executable name. bash is a fixed-address executable and libc is position independent.
    std::vector<Segment> ProcessMemory()
    {
        std::string stack(0x100, '\0');
        const std::string executableName = "/usr/bin/bash";
        memcpy(stack.data() + (c_executableNameAddress - c_stackAddress), executableName.c_str(), executableName.size() + 1);

        return {
            {0x400000, ElfImagePage(c_typeExecutable, 0x400000, c_bashBuildId)},
            {0x7f0000000000, ElfImagePage(c_typeSharedObject, 0, c_libcBuildId, 8)},
            {c_stackAddress, stack},
        };
    }

    std::vector<Note> ProcessNotes(uint64_t ExecutableName = c_executableNameAddress)
    {
        return {
            {"CORE", c_noteProcessStatus, X86_64PrStatus(0x7f0000032345, 11)},
            {"CORE", c_noteSignalInfo, SigInfo(11, 1)},
            {"CORE",
             c_noteAuxiliaryVector,
             AuxiliaryVector({{c_auxProgramHeaders, 0x400040}, {c_auxEntry, 0x401000}, {c_auxExecutableName, ExecutableName}})},
            {"CORE", c_noteMappedFiles, MappedFiles(c_mappings)},
        };
    }

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
        VERIFY_ARE_EQUAL(L"<unknown>", GetExecutableName(""));
        VERIFY_ARE_EQUAL(L"<unknown>", GetExecutableName("!usr!bin!"));
    }

    TEST_METHOD(ParametersMatchRegisteredSignature)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Pid = 42;
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        // Watson rejects the report outright when the parameter count or the names do not match
        // the signature registered for WSLCProcessCrash.
        VERIFY_ARE_EQUAL(static_cast<size_t>(9), c_parameterCount);
        VERIFY_ARE_EQUAL(c_parameterCount, parameters.size());

        VERIFY_ARE_EQUAL(L"ProcessName", parameters[0].first);
        VERIFY_ARE_EQUAL(L"ProcessBuildId", parameters[1].first);
        VERIFY_ARE_EQUAL(L"ModuleName", parameters[2].first);
        VERIFY_ARE_EQUAL(L"ModuleBuildId", parameters[3].first);
        VERIFY_ARE_EQUAL(L"ModuleOffset", parameters[4].first);
        VERIFY_ARE_EQUAL(L"SignalNumber", parameters[5].first);
        VERIFY_ARE_EQUAL(L"SICode", parameters[6].first);
        VERIFY_ARE_EQUAL(L"Architecture", parameters[7].first);
        VERIFY_ARE_EQUAL(L"CallingProcess", parameters[8].first);
    }

    TEST_METHOD(ParametersWithoutACore)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        // Only the core_pattern details are known, everything else is reported as unknown.
        VERIFY_ARE_EQUAL(L"bash", parameters[0].second);
        VERIFY_ARE_EQUAL(L"11", parameters[5].second);
        for (const auto index : {1, 2, 3, 4, 6, 7, 8})
        {
            VERIFY_ARE_EQUAL(L"<unknown>", parameters[index].second);
        }
    }

    TEST_METHOD(CallingProcessAttributesTheCrash)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;
        info.CallingProcess = L"contoso.exe";

        const auto parameters = BuildReportParameters(info);

        VERIFY_ARE_EQUAL(L"contoso.exe", parameters[8].second);
        VERIFY_ARE_EQUAL(L"bash", parameters[0].second);
    }

    TEST_METHOD(ParametersAreLimitedToTheirRegisteredLength)
    {
        LinuxCrashInfo info{};
        info.Executable = std::string(300, 'a');
        info.CallingProcess = std::wstring(300, L'b');

        const auto parameters = BuildReportParameters(info);

        VERIFY_ARE_EQUAL(static_cast<size_t>(128), parameters[0].second.size());
        VERIFY_ARE_EQUAL(static_cast<size_t>(255), parameters[8].second.size());
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
        VERIFY_ARE_EQUAL(std::string("x86_64"), fault.Architecture);
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

        VERIFY_ARE_EQUAL(std::string("aarch64"), fault.Architecture);
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

    TEST_METHOD(CoreReadsProcessDetails)
    {
        const auto core = Read(BuildCore(c_machineX86_64, ProcessNotes(), c_typeCore, ProcessMemory()));

        VERIFY_ARE_EQUAL(11, core.Signal);
        VERIFY_ARE_EQUAL(1, core.SignalCode.value_or(0));
        VERIFY_ARE_EQUAL(std::string("/usr/bin/bash"), core.ExecutablePath);
        VERIFY_ARE_EQUAL(std::string("0123456789abcdef00112233445566778899aabb"), core.ExecutableBuildId);
        VERIFY_ARE_EQUAL(std::string("/usr/lib/x86_64-linux-gnu/libc.so.6"), core.ModulePath);
        VERIFY_ARE_EQUAL(std::string("fedcba9876543210ffeeddccbbaa998877665544"), core.ModuleBuildId);
    }

    TEST_METHOD(CoreWithoutProcessMemory)
    {
        // Without the stack the executable is named by the file mapped at its program headers,
        // and without the ELF header pages there are no build IDs to read.
        const auto core = Read(BuildCore(c_machineX86_64, ProcessNotes()));

        VERIFY_ARE_EQUAL(std::string("/usr/bin/bash"), core.ExecutablePath);
        VERIFY_IS_TRUE(core.ExecutableBuildId.empty());
        VERIFY_IS_TRUE(core.ModuleBuildId.empty());
        VERIFY_ARE_EQUAL(std::string("/usr/lib/x86_64-linux-gnu/libc.so.6"), core.ModulePath);
    }

    TEST_METHOD(CoreIgnoresMalformedModuleImages)
    {
        // A garbage header page, a build ID note with an oversized descriptor and an executable
        // name with no terminator leave those details empty without rejecting the core.
        auto memory = ProcessMemory();
        memory[0].Data.assign(c_pageSize, '\xcc');
        memory[1].Data = ElfImagePage(c_typeSharedObject, 0, std::string(65, '\x01'), 8);
        memory[2].Data.assign(0x100, 'x');

        const auto core = Read(BuildCore(c_machineX86_64, ProcessNotes(), c_typeCore, memory));

        VERIFY_ARE_EQUAL(std::string("/usr/bin/bash"), core.ExecutablePath);
        VERIFY_IS_TRUE(core.ExecutableBuildId.empty());
        VERIFY_IS_TRUE(core.ModuleBuildId.empty());
        VERIFY_ARE_EQUAL(0x32345ull, core.ModuleOffset);
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

        // A signal information descriptor too small to hold si_code.
        VerifyRejected(BuildCore(
            c_machineX86_64, {{"CORE", c_noteProcessStatus, X86_64PrStatus(0x401000)}, {"CORE", c_noteSignalInfo, std::string(8, '\0')}}));
    }

    TEST_METHOD(ParametersDescribeTheCore)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 6;

        CoreDetails core{};
        core.Architecture = "x86_64";
        core.Signal = 11;
        core.SignalCode = -6;
        core.ExecutablePath = "./crash";
        core.ExecutableBuildId = "0123";
        core.ProgramCounter = 0x7f0000032345;
        core.ModulePath = "/usr/lib/x86_64-linux-gnu/libc.so.6";
        core.ModuleBase = 0x7f0000000000;
        core.ModuleOffset = 0x32345;
        core.ModuleBuildId = "4567";

        auto parameters = BuildReportParameters(info, core);
        VERIFY_ARE_EQUAL(L"crash", parameters[0].second);
        VERIFY_ARE_EQUAL(L"0123", parameters[1].second);
        VERIFY_ARE_EQUAL(L"libc.so.6", parameters[2].second);
        VERIFY_ARE_EQUAL(L"4567", parameters[3].second);
        VERIFY_ARE_EQUAL(L"0x32345", parameters[4].second);
        VERIFY_ARE_EQUAL(L"11", parameters[5].second);
        VERIFY_ARE_EQUAL(L"-6", parameters[6].second);
        VERIFY_ARE_EQUAL(L"x86_64", parameters[7].second);

        // Without a module the absolute address is the only meaningful offset.
        core.ModulePath.clear();
        parameters = BuildReportParameters(info, core);
        VERIFY_ARE_EQUAL(L"<unknown>", parameters[2].second);
        VERIFY_ARE_EQUAL(L"<unknown>", parameters[3].second);
        VERIFY_ARE_EQUAL(L"0x7f0000032345", parameters[4].second);

        // Details missing from the core fall back to core_pattern or are reported as unknown.
        core.ExecutablePath.clear();
        core.ExecutableBuildId.clear();
        core.SignalCode.reset();
        parameters = BuildReportParameters(info, core);
        VERIFY_ARE_EQUAL(L"bash", parameters[0].second);
        VERIFY_ARE_EQUAL(L"<unknown>", parameters[1].second);
        VERIFY_ARE_EQUAL(L"<unknown>", parameters[6].second);
    }
};
} // namespace CrashReportingUnitTests
