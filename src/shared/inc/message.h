/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    message.h

Abstract:

    This file contains a utility class to write serialized messages.

--*/

#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <type_traits>
#include <gsl/gsl>
#include <cassert>

namespace wsl::shared {

template <typename TMessage>
class MessageWriter
{
    using THeader = decltype(TMessage::Header);
    using TMessageType = decltype(THeader::MessageType);

public:
    MessageWriter(TMessageType type)
    {
        // Note: this is required because the structure might be padded. For instance:
        // struct a
        // {
        //    char c;
        //    char buffer[0];
        // };
        // Would have a.buffer at byte 1, but sizeof(a) can be > 1 depending on padding.

        const auto bufferOffset = reinterpret_cast<size_t>(&reinterpret_cast<TMessage*>(0)->Buffer);
        m_buffer.resize(bufferOffset);

        (*this)->Header.MessageType = type;
        Size() = static_cast<unsigned long>(bufferOffset);

        // Validate that 'Buffer' has a char type.
        static_assert(std::is_same_v<std::remove_reference_t<decltype((*this)->Buffer[0])>, char>);
    }

    MessageWriter() : MessageWriter(TMessage::Type)
    {
    }

    TMessage* operator->()
    {
        return reinterpret_cast<TMessage*>(m_buffer.data());
    }

    void WriteString(std::string_view String)
    {
        std::transform(String.begin(), String.end(), std::back_inserter(m_buffer), [](auto c) { return static_cast<std::byte>(c); });
        m_buffer.push_back(std::byte{0}); // zero terminator for the string

        Size() = static_cast<unsigned int>(m_buffer.size());
    }

    void WriteString(const char* String)
    {
        WriteString(String ? std::string_view{String} : std::string_view{});
    }

    void WriteString(unsigned int& Index, std::string_view String)
    {
        // Don't write directly to Index since resizing the buffer might invalidate it.
        // Instead save its relative offset to the buffer so we can write there after the resize.
        const auto IndexOffset = GetRelativeIndex(Index);
        const auto IndexValue = Size();

        WriteString(String);
        WriteRelativeIndex(IndexOffset, IndexValue);
    }

    void WriteString(unsigned int& Index, const char* String)
    {
        WriteString(Index, String ? std::string_view{String} : std::string_view{});
    }

    void WriteSpan(const gsl::span<gsl::byte>& Span)
    {
        gsl::copy(Span, InsertBuffer(Span.size()));
    }

    gsl::span<std::byte> InsertBuffer(unsigned int& Index, size_t BufferSize, unsigned int& Size)
    {
        Size = BufferSize;
        return InsertBuffer(Index, BufferSize);
    }

    gsl::span<std::byte> InsertBuffer(unsigned int& Index, size_t BufferSize)
    {
        const auto IndexOffset = GetRelativeIndex(Index);
        const auto IndexValue = Size();

        m_buffer.resize(m_buffer.size() + BufferSize);
        WriteRelativeIndex(IndexOffset, IndexValue);
        Size() = static_cast<unsigned long>(m_buffer.size());

        return Span().subspan(IndexValue, BufferSize);
    }

    gsl::span<std::byte> InsertBuffer(size_t BufferSize)
    {
        m_buffer.resize(m_buffer.size() + BufferSize);
        const auto Index = Size();
        Size() = static_cast<unsigned long>(m_buffer.size());

        return Span().subspan(Index, BufferSize);
    }

    void WriteString(const std::wstring& String)
    {
        WriteString(wsl::shared::string::WideToMultiByte(String));
    }

    void WriteString(const wchar_t* String)
    {
        WriteString(wsl::shared::string::WideToMultiByte(String));
    }

    void WriteString(unsigned int& Index, const std::wstring& String)
    {
        WriteString(Index, wsl::shared::string::WideToMultiByte(String));
    }

    void WriteString(unsigned int& Index, const wchar_t* String)
    {
        WriteString(Index, wsl::shared::string::WideToMultiByte(String));
    }

    gsl::span<std::byte> Span()
    {
        // In case the structure is padded,
        // make sure that the message is at least the size of the structure.

        const int64_t diff = sizeof(TMessage) - m_buffer.size();
        if (diff > 0)
        {
            InsertBuffer(diff);
        }

        return gsl::make_span(m_buffer);
    }

    std::vector<std::byte> MoveBuffer()
    {
        return std::vector<std::byte>(std::move(m_buffer));
    }

private:
    unsigned int& Size()
    {
        return (*this)->Header.MessageSize;
    }

    size_t GetRelativeIndex(unsigned int& Index)
    {
        const size_t Offset = reinterpret_cast<char*>(&Index) - reinterpret_cast<char*>(m_buffer.data());

        // Validate that 'Index' is actually within the bounds of our buffer
        assert(Offset >= 0 && Offset < m_buffer.size());

        return Offset;
    }

    void WriteRelativeIndex(size_t Offset, unsigned int Value)
    {
        *reinterpret_cast<unsigned int*>(&m_buffer[Offset]) = Value;
    }

    std::vector<std::byte> m_buffer;
    size_t m_offset = 0;
};
} // namespace wsl::shared