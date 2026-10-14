// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ElfCore.h"

#include <cstring>
#include <fstream>
#include <optional>
#include <vector>

using wsl::windows::common::elfcore::FaultLocation;

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
constexpr uint32_t c_programHeaderNote = 4;

// When a core has more program headers than fit in e_phnum, the kernel stores PN_XNUM there and
// puts the real count in sh_info of the first section header.
constexpr uint16_t c_programHeaderExtendedCount = 0xffff;

constexpr char c_coreNoteName[] = "CORE";
constexpr uint32_t c_noteProcessStatus = 1;
constexpr uint32_t c_noteMappedFiles = 0x46494c45;

// struct elf_prstatus holds 112 bytes of signal and process state before pr_reg on 64-bit
// targets. pr_reg is user_regs_struct on x86_64, with rip as register 16, and user_pt_regs on
// aarch64, with pc after x0-x30 and sp.
constexpr uint64_t c_prstatusRegistersOffset = 112;
constexpr uint64_t c_x86_64ProgramCounterOffset = c_prstatusRegistersOffset + 16 * sizeof(uint64_t);
constexpr uint64_t c_aarch64ProgramCounterOffset = c_prstatusRegistersOffset + 32 * sizeof(uint64_t);

// The kernel caps NT_FILE at 4MB by default. The ceiling here only bounds what a malformed core
// can make the host allocate.
constexpr uint64_t c_maxMappedFilesNoteSize = 64 * 1024 * 1024;

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

uint64_t AlignNote(uint32_t Size) noexcept
{
    // Linux core notes are 4-byte aligned on every architecture, including 64-bit ones.
    return (static_cast<uint64_t>(Size) + 3) & ~static_cast<uint64_t>(3);
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

void ResolveModule(FaultLocation& Location, const MappedFiles& Files)
{
    const auto& entries = Files.Entries;

    size_t index = 0;
    for (; index < entries.size(); index++)
    {
        if (Location.ProgramCounter >= entries[index].Start && Location.ProgramCounter < entries[index].End)
        {
            break;
        }
    }

    if (index == entries.size())
    {
        return;
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

    Location.ModulePath = path;
    Location.ModuleBase = base;
    Location.ModuleOffset = Location.ProgramCounter - base;
}

} // namespace

FaultLocation wsl::windows::common::elfcore::ReadFaultLocation(_In_ std::istream& Core)
{
    CoreReader reader(Core);

    const auto header = reader.Read<Elf64Header>(0);
    THROW_HR_IF(c_badFormat, memcmp(header.Ident, c_elfMagic, sizeof(c_elfMagic)) != 0);
    THROW_HR_IF(c_badFormat, header.Ident[4] != c_elfClass64 || header.Ident[5] != c_elfDataLittleEndian);
    THROW_HR_IF(c_badFormat, header.Ident[6] != c_elfVersionCurrent || header.Type != c_elfTypeCore);
    THROW_HR_IF(c_badFormat, header.ProgramHeaderEntrySize != sizeof(Elf64ProgramHeader));

    uint64_t programCounterOffset{};
    switch (header.Machine)
    {
    case c_machineX86_64:
        programCounterOffset = c_x86_64ProgramCounterOffset;
        break;
    case c_machineAarch64:
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

    std::optional<uint64_t> programCounter;
    std::optional<MappedFiles> mappedFiles;

    for (uint64_t segment = 0; segment < programHeaderCount && !(programCounter && mappedFiles); segment++)
    {
        const auto programHeader = reader.Read<Elf64ProgramHeader>(header.ProgramHeaderOffset + segment * sizeof(Elf64ProgramHeader));

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

            // The kernel writes the thread that triggered the dump first, so the first
            // NT_PRSTATUS describes the faulting thread.
            if (note.Type == c_noteProcessStatus && !programCounter)
            {
                THROW_HR_IF(c_badFormat, note.DescriptorSize < programCounterOffset + sizeof(uint64_t));
                programCounter = reader.Read<uint64_t>(descriptor + programCounterOffset);
            }
            else if (note.Type == c_noteMappedFiles && !mappedFiles)
            {
                mappedFiles = ParseMappedFiles(reader, descriptor, note.DescriptorSize);
            }
        }
    }

    THROW_HR_IF_MSG(c_badFormat, !programCounter, "Core has no NT_PRSTATUS note");

    FaultLocation location{};
    location.ProgramCounter = programCounter.value();

    if (mappedFiles)
    {
        ResolveModule(location, mappedFiles.value());
    }

    return location;
}

FaultLocation wsl::windows::common::elfcore::ReadFaultLocation(_In_ const std::filesystem::path& Core)
{
    std::ifstream stream(Core, std::ios::binary);
    THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), !stream, "Failed to open core: %ls", Core.c_str());

    return ReadFaultLocation(stream);
}
