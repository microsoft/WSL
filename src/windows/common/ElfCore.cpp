// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ElfCore.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <vector>

using wsl::windows::common::elfcore::CoreDetails;

namespace {

// Layouts from the ELF-64 specification and the Linux core dump format. Every field is naturally
// aligned, so these match the on-disk layout without packing.
struct Elf64Header
{
    unsigned char Ident[16];
    uint16_t Type;
    uint16_t Machine;
    uint32_t Version;
    uint64_t Entry;
    uint64_t ProgramHeaderOffset;
    uint64_t SectionHeaderOffset;
    uint32_t Flags;
    uint16_t HeaderSize;
    uint16_t ProgramHeaderEntrySize;
    uint16_t ProgramHeaderCount;
    uint16_t SectionHeaderEntrySize;
    uint16_t SectionHeaderCount;
    uint16_t SectionNameIndex;
};

struct Elf64ProgramHeader
{
    uint32_t Type;
    uint32_t Flags;
    uint64_t Offset;
    uint64_t VirtualAddress;
    uint64_t PhysicalAddress;
    uint64_t FileSize;
    uint64_t MemorySize;
    uint64_t Alignment;
};

struct Elf64SectionHeader
{
    uint32_t Name;
    uint32_t Type;
    uint64_t Flags;
    uint64_t Address;
    uint64_t Offset;
    uint64_t Size;
    uint32_t Link;
    uint32_t Info;
    uint64_t AddressAlignment;
    uint64_t EntrySize;
};

struct Elf64NoteHeader
{
    uint32_t NameSize;
    uint32_t DescriptorSize;
    uint32_t Type;
};

static_assert(sizeof(Elf64Header) == 64);
static_assert(sizeof(Elf64ProgramHeader) == 56);
static_assert(sizeof(Elf64SectionHeader) == 64);
static_assert(sizeof(Elf64NoteHeader) == 12);

constexpr unsigned char c_elfMagic[] = {0x7f, 'E', 'L', 'F'};
constexpr unsigned char c_elfClass64 = 2;
constexpr unsigned char c_elfDataLittleEndian = 1;
constexpr unsigned char c_elfVersionCurrent = 1;
constexpr uint16_t c_elfTypeCore = 4;
constexpr uint16_t c_machineX86_64 = 62;
constexpr uint16_t c_machineAarch64 = 183;
constexpr uint32_t c_programHeaderLoad = 1;
constexpr uint32_t c_programHeaderNote = 4;

// When a core has more program headers than fit in e_phnum, the kernel stores PN_XNUM there and
// puts the real count in sh_info of the first section header.
constexpr uint16_t c_programHeaderExtendedCount = 0xffff;

constexpr char c_coreNoteName[] = "CORE";
constexpr uint32_t c_noteProcessStatus = 1;
constexpr uint32_t c_noteAuxiliaryVector = 6;
constexpr uint32_t c_noteSignalInfo = 0x53494749;
constexpr uint32_t c_noteMappedFiles = 0x46494c45;

constexpr char c_gnuNoteName[] = "GNU";
constexpr uint32_t c_noteGnuBuildId = 3;

constexpr uint64_t c_auxNull = 0;
constexpr uint64_t c_auxProgramHeaders = 3;
constexpr uint64_t c_auxEntry = 9;
constexpr uint64_t c_auxExecutableName = 31;

// siginfo_t starts with si_signo, si_errno and si_code, each an int.
constexpr uint64_t c_siginfoCodeOffset = 2 * sizeof(int32_t);

// struct elf_prstatus holds 112 bytes of signal and process state before pr_reg on 64-bit
// targets. pr_reg is user_regs_struct on x86_64, with rip as register 16, and user_pt_regs on
// aarch64, with pc after x0-x30 and sp.
constexpr uint64_t c_prstatusRegistersOffset = 112;
constexpr uint64_t c_x86_64ProgramCounterOffset = c_prstatusRegistersOffset + 16 * sizeof(uint64_t);
constexpr uint64_t c_aarch64ProgramCounterOffset = c_prstatusRegistersOffset + 32 * sizeof(uint64_t);

// The kernel caps NT_FILE at 4MB by default. The ceiling here only bounds what a malformed core
// can make the host allocate.
constexpr uint64_t c_maxMappedFilesNoteSize = 64 * 1024 * 1024;

// Bounds on what is read from the process image. The auxiliary vector holds a few dozen entries,
// build IDs are 16 or 20 bytes in practice, and PATH_MAX is 4096.
constexpr uint64_t c_maxAuxiliaryVectorSize = 64 * 1024;
constexpr size_t c_maxPathLength = 4096;
constexpr uint16_t c_maxModuleProgramHeaders = 256;
constexpr uint64_t c_maxModuleNoteSize = 64 * 1024;
constexpr uint32_t c_maxBuildIdSize = 64;

const HRESULT c_badFormat = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

struct MappedFile
{
    uint64_t Start;
    uint64_t End;
    uint64_t FileOffsetPages;
    std::string Path;
};

struct MappedFiles
{
    uint64_t PageSize{};
    std::vector<MappedFile> Entries;
};

// Bounds-checked random access over the core. Every read is validated against the stream size
// before seeking, so a malformed offset or length fails instead of reading past the end.
class CoreReader
{
public:
    explicit CoreReader(std::istream& Stream) : m_stream(Stream)
    {
        m_stream.seekg(0, std::ios::end);
        const auto size = m_stream.tellg();
        THROW_HR_IF(c_badFormat, !m_stream || size < 0);

        m_size = static_cast<uint64_t>(size);
    }

    uint64_t Size() const noexcept
    {
        return m_size;
    }

    void Read(uint64_t Offset, void* Buffer, size_t Length)
    {
        THROW_HR_IF(c_badFormat, Offset > m_size || Length > m_size - Offset);

        m_stream.clear();
        m_stream.seekg(static_cast<std::streamoff>(Offset));
        m_stream.read(static_cast<char*>(Buffer), static_cast<std::streamsize>(Length));
        THROW_HR_IF(c_badFormat, m_stream.gcount() != static_cast<std::streamsize>(Length));
    }

    template <typename T>
    T Read(uint64_t Offset)
    {
        T value{};
        Read(Offset, &value, sizeof(value));
        return value;
    }

private:
    std::istream& m_stream;
    uint64_t m_size{};
};

// The process memory captured by the core's PT_LOAD segments. The kernel leaves out memory it
// chooses not to dump, so lookups report whether the whole range was captured instead of throwing.
class CoreMemory
{
public:
    explicit CoreMemory(CoreReader& Reader) : m_reader(Reader)
    {
    }

    void Add(const Elf64ProgramHeader& Segment)
    {
        if (Segment.FileSize != 0)
        {
            m_segments.push_back(Segment);
        }
    }

    bool Read(uint64_t Address, void* Buffer, size_t Length)
    {
        const auto available = Available(Address);
        if (!available || Length > available->second)
        {
            return false;
        }

        m_reader.Read(available->first, Buffer, Length);
        return true;
    }

    // Reads a NUL-terminated string of at most MaxLength characters. Returns an empty string when
    // the terminator is not within the captured memory.
    std::string ReadString(uint64_t Address, size_t MaxLength)
    {
        const auto available = Available(Address);
        if (!available)
        {
            return {};
        }

        std::vector<char> buffer(static_cast<size_t>(std::min<uint64_t>(available->second, MaxLength + 1)));
        m_reader.Read(available->first, buffer.data(), buffer.size());

        const auto terminator = std::find(buffer.begin(), buffer.end(), '\0');
        if (terminator == buffer.end())
        {
            return {};
        }

        return std::string(buffer.begin(), terminator);
    }

private:
    // Returns the core file offset of Address and how many bytes from there are in the core.
    std::optional<std::pair<uint64_t, uint64_t>> Available(uint64_t Address) const
    {
        const auto size = m_reader.Size();
        for (const auto& segment : m_segments)
        {
            if (Address < segment.VirtualAddress || Address - segment.VirtualAddress >= segment.FileSize)
            {
                continue;
            }

            // Cores are truncated when they exceed the size limit, leaving segments that point
            // past the end of the file.
            const auto offset = Address - segment.VirtualAddress;
            if (segment.Offset > size || offset >= size - segment.Offset)
            {
                return {};
            }

            const auto inSegment = segment.FileSize - offset;
            const auto inFile = size - segment.Offset - offset;
            return std::make_pair(segment.Offset + offset, std::min(inSegment, inFile));
        }

        return {};
    }

    CoreReader& m_reader;
    std::vector<Elf64ProgramHeader> m_segments;
};

uint64_t AlignNote(uint32_t Size, uint64_t Alignment = 4) noexcept
{
    // Linux core notes are 4-byte aligned on every architecture, including 64-bit ones. Notes in
    // binaries follow the alignment of their PT_NOTE segment, which is 4 or 8.
    return (static_cast<uint64_t>(Size) + Alignment - 1) & ~(Alignment - 1);
}

uint64_t ReadUInt64(const std::vector<char>& Buffer, size_t Offset)
{
    uint64_t value{};
    memcpy(&value, Buffer.data() + Offset, sizeof(value));
    return value;
}

// NT_FILE: a count and page size, then that many (start, end, file offset in pages) triples, then
// that many NUL-terminated paths in the same order.
MappedFiles ParseMappedFiles(CoreReader& Reader, uint64_t Offset, uint32_t Size)
{
    constexpr size_t headerSize = 2 * sizeof(uint64_t);
    constexpr size_t entrySize = 3 * sizeof(uint64_t);

    THROW_HR_IF(c_badFormat, Size < headerSize || Size > c_maxMappedFilesNoteSize);

    std::vector<char> buffer(Size);
    Reader.Read(Offset, buffer.data(), buffer.size());

    MappedFiles files;
    const auto count = ReadUInt64(buffer, 0);
    files.PageSize = ReadUInt64(buffer, sizeof(uint64_t));
    THROW_HR_IF(c_badFormat, count > (buffer.size() - headerSize) / entrySize);

    files.Entries.reserve(static_cast<size_t>(count));

    size_t names = headerSize + static_cast<size_t>(count) * entrySize;
    for (size_t index = 0; index < count; index++)
    {
        const auto entry = headerSize + index * entrySize;

        const auto* name = buffer.data() + names;
        const auto* terminator = static_cast<const char*>(memchr(name, '\0', buffer.size() - names));
        THROW_HR_IF(c_badFormat, terminator == nullptr);

        files.Entries.push_back(
            {ReadUInt64(buffer, entry),
             ReadUInt64(buffer, entry + sizeof(uint64_t)),
             ReadUInt64(buffer, entry + 2 * sizeof(uint64_t)),
             std::string(name, terminator)});

        names += (terminator - name) + 1;
    }

    return files;
}

struct Module
{
    std::string Path;
    uint64_t Base;
};

std::optional<Module> FindModule(const MappedFiles& Files, uint64_t Address)
{
    const auto& entries = Files.Entries;

    size_t index = 0;
    for (; index < entries.size(); index++)
    {
        if (Address >= entries[index].Start && Address < entries[index].End)
        {
            break;
        }
    }

    if (index == entries.size())
    {
        return {};
    }

    // A module is mapped as one entry per segment, listed in address order. Its load address is
    // that of the first segment, less the file offset that segment was mapped from.
    const auto& path = entries[index].Path;
    while (index > 0 && entries[index - 1].Path == path)
    {
        index--;
    }

    const auto& first = entries[index];
    uint64_t base = first.Start;
    if (Files.PageSize != 0 && first.FileOffsetPages <= first.Start / Files.PageSize)
    {
        base -= first.FileOffsetPages * Files.PageSize;
    }

    return Module{path, base};
}

std::string ToHex(const char* Bytes, size_t Length)
{
    constexpr char digits[] = "0123456789abcdef";

    std::string hex;
    hex.reserve(Length * 2);
    for (size_t index = 0; index < Length; index++)
    {
        const auto byte = static_cast<unsigned char>(Bytes[index]);
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0xf]);
    }

    return hex;
}

std::string FindBuildIdNote(const std::vector<char>& Notes, uint64_t Alignment)
{
    size_t position = 0;
    while (Notes.size() - position >= sizeof(Elf64NoteHeader))
    {
        Elf64NoteHeader note{};
        memcpy(&note, Notes.data() + position, sizeof(note));
        position += sizeof(note);

        const auto nameSize = AlignNote(note.NameSize, Alignment);
        const auto descriptorSize = AlignNote(note.DescriptorSize, Alignment);
        if (nameSize > Notes.size() - position || note.DescriptorSize > Notes.size() - position - nameSize)
        {
            return {};
        }

        const auto* name = Notes.data() + position;
        const auto* descriptor = name + nameSize;

        if (note.Type == c_noteGnuBuildId && note.NameSize == sizeof(c_gnuNoteName) &&
            memcmp(name, c_gnuNoteName, sizeof(c_gnuNoteName)) == 0 && note.DescriptorSize > 0 && note.DescriptorSize <= c_maxBuildIdSize)
        {
            return ToHex(descriptor, note.DescriptorSize);
        }

        if (descriptorSize > Notes.size() - position - nameSize)
        {
            return {};
        }

        position += nameSize + descriptorSize;
    }

    return {};
}

// Reads the GNU build ID of the ELF image loaded at Base. The kernel dumps the first page of every
// mapped ELF file by default, which holds the ELF and program headers and, in practice, the build
// ID note. Returns an empty string when any of it is missing from the core or malformed.
std::string ReadBuildId(CoreMemory& Memory, uint64_t Base)
{
    Elf64Header header{};
    if (!Memory.Read(Base, &header, sizeof(header)) || memcmp(header.Ident, c_elfMagic, sizeof(c_elfMagic)) != 0 ||
        header.Ident[4] != c_elfClass64 || header.Ident[5] != c_elfDataLittleEndian ||
        header.ProgramHeaderEntrySize != sizeof(Elf64ProgramHeader) || header.ProgramHeaderCount == 0 ||
        header.ProgramHeaderCount > c_maxModuleProgramHeaders || header.ProgramHeaderOffset > UINT64_MAX - Base)
    {
        return {};
    }

    std::vector<Elf64ProgramHeader> programHeaders(header.ProgramHeaderCount);
    if (!Memory.Read(Base + header.ProgramHeaderOffset, programHeaders.data(), programHeaders.size() * sizeof(Elf64ProgramHeader)))
    {
        return {};
    }

    // Segments are loaded at their virtual address plus a bias that is zero for fixed-address
    // executables. The lowest PT_LOAD maps the start of the file, which is where Base points.
    const Elf64ProgramHeader* lowest = nullptr;
    for (const auto& programHeader : programHeaders)
    {
        if (programHeader.Type == c_programHeaderLoad && (lowest == nullptr || programHeader.VirtualAddress < lowest->VirtualAddress))
        {
            lowest = &programHeader;
        }
    }

    if (lowest == nullptr)
    {
        return {};
    }

    // Unsigned wraparound is intended: the bias is applied modulo 2^64.
    const auto bias = Base - (lowest->VirtualAddress - lowest->Offset);

    for (const auto& programHeader : programHeaders)
    {
        if (programHeader.Type != c_programHeaderNote || programHeader.FileSize < sizeof(Elf64NoteHeader))
        {
            continue;
        }

        // Read the notes as loaded rather than from Base plus their file offset, so that a note
        // the loader placed elsewhere is still found.
        std::vector<char> notes(static_cast<size_t>(std::min(programHeader.FileSize, c_maxModuleNoteSize)));
        if (!Memory.Read(programHeader.VirtualAddress + bias, notes.data(), notes.size()))
        {
            continue;
        }

        auto buildId = FindBuildIdNote(notes, programHeader.Alignment == 8 ? 8 : 4);
        if (!buildId.empty())
        {
            return buildId;
        }
    }

    return {};
}

struct AuxiliaryVector
{
    std::optional<uint64_t> ProgramHeaders;
    std::optional<uint64_t> Entry;
    std::optional<uint64_t> ExecutableName;
};

// NT_AUXV: the process's auxiliary vector, (type, value) pairs ending with AT_NULL.
AuxiliaryVector ParseAuxiliaryVector(CoreReader& Reader, uint64_t Offset, uint32_t Size)
{
    constexpr size_t entrySize = 2 * sizeof(uint64_t);

    THROW_HR_IF(c_badFormat, Size > c_maxAuxiliaryVectorSize);

    std::vector<char> buffer(Size);
    Reader.Read(Offset, buffer.data(), buffer.size());

    AuxiliaryVector vector;
    for (size_t entry = 0; buffer.size() - entry >= entrySize; entry += entrySize)
    {
        const auto type = ReadUInt64(buffer, entry);
        const auto value = ReadUInt64(buffer, entry + sizeof(uint64_t));

        switch (type)
        {
        case c_auxNull:
            return vector;
        case c_auxProgramHeaders:
            vector.ProgramHeaders = value;
            break;
        case c_auxEntry:
            vector.Entry = value;
            break;
        case c_auxExecutableName:
            vector.ExecutableName = value;
            break;
        default:
            break;
        }
    }

    return vector;
}

} // namespace

CoreDetails wsl::windows::common::elfcore::ReadCoreDetails(_In_ std::istream& Core)
{
    CoreReader reader(Core);

    const auto header = reader.Read<Elf64Header>(0);
    THROW_HR_IF(c_badFormat, memcmp(header.Ident, c_elfMagic, sizeof(c_elfMagic)) != 0);
    THROW_HR_IF(c_badFormat, header.Ident[4] != c_elfClass64 || header.Ident[5] != c_elfDataLittleEndian);
    THROW_HR_IF(c_badFormat, header.Ident[6] != c_elfVersionCurrent || header.Type != c_elfTypeCore);
    THROW_HR_IF(c_badFormat, header.ProgramHeaderEntrySize != sizeof(Elf64ProgramHeader));

    CoreDetails details{};
    uint64_t programCounterOffset{};
    switch (header.Machine)
    {
    case c_machineX86_64:
        details.Architecture = "x86_64";
        programCounterOffset = c_x86_64ProgramCounterOffset;
        break;
    case c_machineAarch64:
        details.Architecture = "aarch64";
        programCounterOffset = c_aarch64ProgramCounterOffset;
        break;
    default:
        THROW_HR(c_badFormat);
    }

    uint64_t programHeaderCount = header.ProgramHeaderCount;
    if (programHeaderCount == c_programHeaderExtendedCount)
    {
        THROW_HR_IF(c_badFormat, header.SectionHeaderOffset == 0);
        programHeaderCount = reader.Read<Elf64SectionHeader>(header.SectionHeaderOffset).Info;
    }

    THROW_HR_IF(
        c_badFormat,
        header.ProgramHeaderOffset > reader.Size() ||
            programHeaderCount > (reader.Size() - header.ProgramHeaderOffset) / sizeof(Elf64ProgramHeader));

    CoreMemory memory(reader);
    bool foundProcessStatus = false;
    std::optional<MappedFiles> mappedFiles;
    std::optional<AuxiliaryVector> auxiliaryVector;

    for (uint64_t segment = 0; segment < programHeaderCount; segment++)
    {
        const auto programHeader = reader.Read<Elf64ProgramHeader>(header.ProgramHeaderOffset + segment * sizeof(Elf64ProgramHeader));

        if (programHeader.Type == c_programHeaderLoad)
        {
            memory.Add(programHeader);
            continue;
        }

        if (programHeader.Type != c_programHeaderNote)
        {
            continue;
        }

        THROW_HR_IF(c_badFormat, programHeader.Offset > reader.Size() || programHeader.FileSize > reader.Size() - programHeader.Offset);

        auto position = programHeader.Offset;
        const auto end = programHeader.Offset + programHeader.FileSize;
        while (end - position >= sizeof(Elf64NoteHeader))
        {
            const auto note = reader.Read<Elf64NoteHeader>(position);
            position += sizeof(note);

            const auto nameSize = AlignNote(note.NameSize);
            const auto descriptorSize = AlignNote(note.DescriptorSize);
            THROW_HR_IF(c_badFormat, nameSize > end - position || descriptorSize > end - position - nameSize);

            bool isCoreNote = false;
            if (note.NameSize == sizeof(c_coreNoteName))
            {
                char name[sizeof(c_coreNoteName)]{};
                reader.Read(position, name, sizeof(name));
                isCoreNote = memcmp(name, c_coreNoteName, sizeof(name)) == 0;
            }

            const auto descriptor = position + nameSize;
            position = descriptor + descriptorSize;

            if (!isCoreNote)
            {
                continue;
            }

            switch (note.Type)
            {
            case c_noteProcessStatus:
                // The kernel writes the thread that triggered the dump first, so the first
                // NT_PRSTATUS describes the faulting thread. It starts with pr_info.si_signo.
                if (!foundProcessStatus)
                {
                    THROW_HR_IF(c_badFormat, note.DescriptorSize < programCounterOffset + sizeof(uint64_t));
                    details.Signal = reader.Read<int32_t>(descriptor);
                    details.ProgramCounter = reader.Read<uint64_t>(descriptor + programCounterOffset);
                    foundProcessStatus = true;
                }
                break;

            case c_noteSignalInfo:
                if (!details.SignalCode)
                {
                    THROW_HR_IF(c_badFormat, note.DescriptorSize < c_siginfoCodeOffset + sizeof(int32_t));
                    details.SignalCode = reader.Read<int32_t>(descriptor + c_siginfoCodeOffset);
                }
                break;

            case c_noteAuxiliaryVector:
                if (!auxiliaryVector)
                {
                    auxiliaryVector = ParseAuxiliaryVector(reader, descriptor, note.DescriptorSize);
                }
                break;

            case c_noteMappedFiles:
                if (!mappedFiles)
                {
                    mappedFiles = ParseMappedFiles(reader, descriptor, note.DescriptorSize);
                }
                break;

            default:
                break;
            }
        }
    }

    THROW_HR_IF_MSG(c_badFormat, !foundProcessStatus, "Core has no NT_PRSTATUS note");

    if (auxiliaryVector && auxiliaryVector->ExecutableName)
    {
        details.ExecutablePath = memory.ReadString(auxiliaryVector->ExecutableName.value(), c_maxPathLength);
    }

    if (!mappedFiles)
    {
        return details;
    }

    if (const auto module = FindModule(mappedFiles.value(), details.ProgramCounter))
    {
        details.ModulePath = module->Path;
        details.ModuleBase = module->Base;
        details.ModuleOffset = details.ProgramCounter - module->Base;
        details.ModuleBuildId = ReadBuildId(memory, module->Base);
    }

    // The program headers and entry point both lie inside the main executable's mappings.
    std::optional<Module> executable;
    if (auxiliaryVector && auxiliaryVector->ProgramHeaders)
    {
        executable = FindModule(mappedFiles.value(), auxiliaryVector->ProgramHeaders.value());
    }

    if (!executable && auxiliaryVector && auxiliaryVector->Entry)
    {
        executable = FindModule(mappedFiles.value(), auxiliaryVector->Entry.value());
    }

    if (executable)
    {
        if (details.ExecutablePath.empty())
        {
            details.ExecutablePath = executable->Path;
        }

        details.ExecutableBuildId = ReadBuildId(memory, executable->Base);
    }

    return details;
}

CoreDetails wsl::windows::common::elfcore::ReadCoreDetails(_In_ const std::filesystem::path& Core)
{
    std::ifstream stream(Core, std::ios::binary);
    THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), !stream, "Failed to open core: %ls", Core.c_str());

    return ReadCoreDetails(stream);
}
