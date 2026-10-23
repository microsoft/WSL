// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Common.h"

// Builds ustar archives in memory, including names that cannot exist as Windows files.
class TarBuilder
{
public:
    TarBuilder& AddFile(std::string_view Name, std::string_view Content)
    {
        AddEntry(Name, '0', Content, {});
        return *this;
    }

    TarBuilder& AddDirectory(std::string_view Name)
    {
        AddEntry(std::string{Name} + "/", '5', {}, {});
        return *this;
    }

    TarBuilder& AddHardLink(std::string_view Name, std::string_view Target)
    {
        AddEntry(Name, '1', {}, Target);
        return *this;
    }

    TarBuilder& AddSymlink(std::string_view Name, std::string_view Target)
    {
        AddEntry(Name, '2', {}, Target);
        return *this;
    }

    // An archive ends with two zero blocks. An archive with no entries is just those blocks.
    std::vector<char> Build() const
    {
        auto bytes = m_bytes;
        bytes.resize(bytes.size() + (2 * c_blockSize), '\0');
        return bytes;
    }

    // Matches the callback the extraction helpers invoke to obtain the archive.
    std::function<void(HANDLE)> Writer() const
    {
        return [bytes = Build()](HANDLE Handle) {
            DWORD written = 0;
            VERIFY_IS_TRUE(!!WriteFile(Handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
            VERIFY_ARE_EQUAL(bytes.size(), static_cast<size_t>(written));
        };
    }

private:
    static constexpr size_t c_blockSize = 512;

    void AddEntry(std::string_view Name, char TypeFlag, std::string_view Content, std::string_view LinkTarget)
    {
        const auto headerOffset = m_bytes.size();
        m_bytes.resize(headerOffset + c_blockSize, '\0');
        auto* header = m_bytes.data() + headerOffset;

        WriteText(header + 0, 100, Name);
        WriteOctal(header + 100, 8, TypeFlag == '5' ? 0755 : 0644);
        WriteOctal(header + 108, 8, 0);
        WriteOctal(header + 116, 8, 0);
        WriteOctal(header + 124, 12, Content.size());
        WriteOctal(header + 136, 12, 0);
        header[156] = TypeFlag;
        WriteText(header + 157, 100, LinkTarget);
        std::memcpy(header + 257, "ustar", 5);
        std::memcpy(header + 263, "00", 2);

        // The checksum covers the whole header with its own field read as spaces.
        std::memset(header + 148, ' ', 8);
        unsigned int sum = 0;
        for (size_t i = 0; i < c_blockSize; ++i)
        {
            sum += static_cast<unsigned char>(header[i]);
        }

        const auto checksum = std::format("{:06o}", sum);
        std::memcpy(header + 148, checksum.data(), checksum.size());
        header[154] = '\0';
        header[155] = ' ';

        // Content is padded out to a whole number of blocks.
        if (!Content.empty())
        {
            const auto padded = ((Content.size() + c_blockSize - 1) / c_blockSize) * c_blockSize;
            const auto contentOffset = m_bytes.size();
            m_bytes.resize(contentOffset + padded, '\0');
            std::memcpy(m_bytes.data() + contentOffset, Content.data(), Content.size());
        }
    }

    static void WriteText(char* Field, size_t Size, std::string_view Value)
    {
        VERIFY_IS_TRUE(Value.size() < Size);
        std::memcpy(Field, Value.data(), Value.size());
    }

    static void WriteOctal(char* Field, size_t Size, size_t Value)
    {
        const auto text = std::format("{:0{}o}", Value, Size - 1);
        VERIFY_IS_TRUE(text.size() < Size);
        std::memcpy(Field, text.data(), text.size());
    }

    std::vector<char> m_bytes;
};
