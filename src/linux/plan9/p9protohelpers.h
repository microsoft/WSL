// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9defs.h"

namespace p9fs {

struct DirectoryEntry
{
    Qid Qid;
    UINT64 Offset;
    UINT8 Type;
    std::string_view Name;
};

template <typename T>
struct ReadResult
{
    T Result;
    bool Success;
};

// Class to read elements from a 9pfs protocol buffer.
// TODO: Need ways to read that don't fail fast but return errors.
class SpanReader
{
public:
    SpanReader() = default;

    SpanReader(gsl::span<const gsl::byte> message) : m_Message(message)
    {
    }

    gsl::span<const gsl::byte> Read(unsigned int count)
    {
        if (m_Message.size() - m_Offset < count)
        {
            FAIL_FAST();
        }

        auto result = m_Message.subspan(m_Offset, count);
        m_Offset += count;
        return result;
    }

    ReadResult<gsl::span<const gsl::byte>> TryRead(unsigned int count)
    {
        if (m_Message.size() - m_Offset < count)
        {
            return {};
        }

        auto result = m_Message.subspan(m_Offset, count);
        m_Offset += count;
        return {result, true};
    }

    UINT8 U8()
    {
        return ReadFixed<UINT8>();
    }

    UINT16 U16()
    {
        return ReadFixed<UINT16>();
    }

    UINT32 U32()
    {
        return ReadFixed<UINT32>();
    }

    UINT64 U64()
    {
        return ReadFixed<UINT64>();
    }

    ReadResult<UINT8> TryU8()
    {
        return TryReadFixed<UINT8>();
    }

    ReadResult<UINT16> TryU16()
    {
        return TryReadFixed<UINT16>();
    }

    ReadResult<UINT32> TryU32()
    {
        return TryReadFixed<UINT32>();
    }

    ReadResult<UINT64> TryU64()
    {
        return TryReadFixed<UINT64>();
    }

    Qid Qid()
    {
        p9fs::Qid result;
        result.Type = static_cast<QidType>(U8());
        result.Version = U32();
        result.Path = U64();
        return result;
    }

    ReadResult<p9fs::Qid> TryQid()
    {
        if (m_Message.size() - m_Offset < QidSize)
        {
            return {};
        }

        return {Qid(), true};
    }

    std::string_view String()
    {
        const auto length = U16();
        auto s = Read(length);
        return FixString(s);
    }

    ReadResult<std::string_view> TryString()
    {
        const auto length = TryU16();
        if (!length.Success)
        {
            return {};
        }

        auto s = TryRead(length.Result);
        if (!s.Success)
        {
            return {};
        }

        return {FixString(s.Result), true};
    }

#ifndef GSL_KERNEL_MODE

    std::string_view Name()
    {
        auto s = String();
        if (s.size() == 0 || s == "." || s == ".." || std::find_if(s.begin(), s.end(), [](char c) { return c == '/'; }) != s.end())
        {
            THROW_INVALID();
        }

        return s;
    }

#endif

    ReadResult<DirectoryEntry> TryDirectoryEntry()
    {
        // Check if the data is large enough for the fixed part.
        if (m_Message.size() - m_Offset < QidSize + sizeof(UINT64) + sizeof(UINT8) + sizeof(UINT16))
        {
            return {};
        }

        DirectoryEntry result;
        result.Qid = Qid();
        result.Offset = U64();
        result.Type = U8();
        auto name = TryString();
        if (!name.Success)
        {
            return {};
        }

        result.Name = name.Result;
        return {result, true};
    }

    StatResult ReadStatResult()
    {
        StatResult attr;
        attr.Mode = U32();
        attr.Uid = U32();
        attr.Gid = U32();
        attr.NLink = U64();
        attr.RDev = U64();
        attr.Size = U64();
        attr.BlockSize = U64();
        attr.Blocks = U64();
        attr.AtimeSec = U64();
        attr.AtimeNsec = U64();
        attr.MtimeSec = U64();
        attr.MtimeNsec = U64();
        attr.CtimeSec = U64();
        attr.CtimeNsec = U64();
        return attr;
    }

    ReadResult<StatResult> TryStatResult()
    {
        // Check if the data is large enough.
        if (m_Message.size() - m_Offset < StatResultSize)
        {
            return {};
        }

        return {ReadStatResult(), true};
    }

    size_t Size() const
    {
        return m_Message.size();
    }

    size_t Offset() const
    {
        return m_Offset;
    }

    gsl::span<const gsl::byte> ReadToEnd()
    {
        return Read(static_cast<unsigned int>(m_Message.size() - m_Offset));
    }

    gsl::span<const gsl::byte> Span() const
    {
        return m_Message;
    }

private:
    template <class T>
    T ReadFixed()
    {
        auto s = Read(sizeof(T));
        return *reinterpret_cast<const T*>(s.data());
    }

    template <typename T>
    ReadResult<T> TryReadFixed()
    {
        auto s = TryRead(sizeof(T));
        if (!s.Success)
        {
            return {};
        }

        return {*reinterpret_cast<const T*>(s.Result.data()), true};
    }

    std::string_view FixString(gsl::span<const gsl::byte> s)
    {
        auto string = std::string_view{reinterpret_cast<const char*>(s.data()), static_cast<std::string_view::size_type>(s.size())};

        //
        // Check for internal nul characters.
        //
        std::string_view::size_type strlength = 0;
        while (strlength < string.size() && string[strlength] != 0)
        {
            strlength++;
        }

        return string.substr(0, strlength);
    }

    gsl::span<const gsl::byte> m_Message;
    size_t m_Offset{};
};

// Class to write elements to a 9pfs protocol buffer.
class SpanWriter
{
public:
    SpanWriter(gsl::span<gsl::byte> message) : Message(message)
    {
    }

    void U8(UINT8 value)
    {
        WriteFixed(value);
    }

    void U16(UINT16 value)
    {
        WriteFixed(value);
    }

    void U32(UINT32 value)
    {
        WriteFixed(value);
    }

    void U64(UINT64 value)
    {
        WriteFixed(value);
    }

    void Qid(const Qid& value)
    {
        U8(static_cast<UINT8>(value.Type));
        U32(value.Version);
        U64(value.Path);
    }

    void String(std::string_view value)
    {
        if (value.size() > UINT16_MAX)
        {
            FAIL_FAST();
        }

        U16(static_cast<UINT16>(value.size()));
        auto s = Next(value.size());
        auto bytes = gsl::as_bytes(gsl::make_span(value.data(), value.size()));
        gsl::copy(bytes, s);
    }

    gsl::span<gsl::byte> Result()
    {
        return Message.subspan(0, Offset);
    }

    size_t Size() const
    {
        return Offset;
    }

    size_t MaxSize() const
    {
        return Message.size();
    }

    gsl::span<gsl::byte> Peek()
    {
        return Message.subspan(Offset);
    }

    gsl::span<gsl::byte> Peek(size_t Count)
    {
        return Message.subspan(Offset, Count);
    }

    gsl::span<gsl::byte> Next(size_t Count)
    {
        auto s = Peek(Count);
        Offset += Count;
        return s;
    }

    void Header(MessageType messageType, UINT16 tag) const
    {
        SpanWriter headerWriter{Message.subspan(0, HeaderSize)};
        headerWriter.U32(static_cast<UINT32>(Offset));
        headerWriter.U8(static_cast<UINT8>(messageType));
        headerWriter.U16(tag);
    }

    void Write(gsl::span<const gsl::byte> buffer)
    {
        gsl::copy(buffer, Next(buffer.size()));
    }

private:
    template <class T>
    void WriteFixed(T value)
    {
        auto s = Next(sizeof(value));
        *reinterpret_cast<T*>(s.data()) = value;
    }

    gsl::span<gsl::byte> Message;
    size_t Offset{};
};

} // namespace p9fs
