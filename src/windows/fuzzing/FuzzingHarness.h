// Copyright (C) Microsoft Corporation. All rights reserved.

// Shared utilities for fuzzing harnesses.
// Provides:
//  * FuzzInput - a cursor over the raw libFuzzer buffer
//  * main() - for replaying corpus inputs without the libFuzzer runtime (e.g. for debugging or OneFuzz repros)

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Cursor over fuzz input bytes. Harnesses construct this from LLVMFuzzerTestOneInput args
// and call typed Read methods. All reads are bounds-checked and return default values on exhaustion.
class FuzzInput
{
public:
    FuzzInput(const uint8_t* data, size_t size) : m_data(data), m_size(size), m_offset(0)
    {
    }

    bool Empty() const
    {
        return m_offset >= m_size;
    }

    size_t Remaining() const
    {
        return m_offset < m_size ? m_size - m_offset : 0;
    }

    // Read a fixed-size value (uint8_t, uint16_t, uint32_t, etc.)
    template <typename T>
    T Read()
    {
        T value{};
        if (m_offset + sizeof(T) <= m_size)
        {
            std::memcpy(&value, m_data + m_offset, sizeof(T));
            m_offset += sizeof(T);
        }
        return value;
    }

    // Read a null-terminated narrow string (up to maxLen chars)
    std::string ReadString(size_t maxLen = 64)
    {
        std::string result;
        while (m_offset < m_size && result.size() < maxLen)
        {
            char ch = static_cast<char>(m_data[m_offset++]);
            if (ch == '\0')
            {
                break;
            }
            result.push_back(ch);
        }
        return result;
    }

    // Read a null-terminated wide string (byte pairs as little-endian wchar_t, up to maxLen chars)
    std::wstring ReadWideString(size_t maxLen = 64)
    {
        std::wstring result;
        while (m_offset + 1 < m_size && result.size() < maxLen)
        {
            wchar_t ch = static_cast<wchar_t>(m_data[m_offset] | (m_data[m_offset + 1] << 8));
            m_offset += 2;
            if (ch == L'\0')
            {
                break;
            }
            result.push_back(ch);
        }
        return result;
    }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_offset;
};

#ifdef WSL_ENABLE_FUZZING_REPLAY

#include <filesystem>
#include <fstream>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv);

// Replay mode: run the harness on each argument as a corpus file.
int main(int argc, char** argv)
{
    LLVMFuzzerInitialize(&argc, &argv);
    for (int i = 1; i < argc; ++i)
    {
        std::ifstream stream(argv[i], std::ios_base::in | std::ios_base::binary);
        std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
    }
    return 0;
}

#endif // WSL_ENABLE_FUZZING_REPLAY
