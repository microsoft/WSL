//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//

/*++

Module Name:

    prettyprintshared.h

Abstract:

    This is the header file for message pretty printing.

--*/

#pragma once

#include <sstream>
#include <cstring>
#include <string_view>

#include "defs.h"
#include "stringshared.h"

#ifdef WIN32
#include <guiddef.h>
#else
#include "lxdef.h"
#endif

#define FIELD(Name) #Name, Name

#define STRING_FIELD(Name) #Name, (Name <= 0 ? "<empty>" : ((char*)(this)) + Name)

#define STRING_ARRAY_FIELD(Name) #Name, (StringArray((char*)(this), Name, Header.MessageSize))

// Safe pretty-print for flexible array members (char Buffer[]). Bounds the read
// using the struct's Header.MessageSize so it never reads past the received data.
#define BUFFER_FIELD(Name) #Name, PrettyPrintSafeBufferView(this, Header.MessageSize, Name)

inline std::string_view PrettyPrintSafeBufferView(const void* structBase, unsigned int messageSize, const char* buffer)
{
    const auto offset = static_cast<size_t>(buffer - reinterpret_cast<const char*>(structBase));
    if (offset >= messageSize)
    {
        return "<out-of-bounds>";
    }

    const size_t maxLen = messageSize - offset;
    return std::string_view(buffer, strnlen(buffer, maxLen));
}

#define PRETTY_PRINT(...) \
    void PrettyPrintImpl(std::stringstream& Out) const \
    { \
        PrettyPrintField(Out, __VA_ARGS__); \
    } \
\
    std::string PrettyPrint() const \
    { \
        std::stringstream Out; \
        PrettyPrintImpl(Out); \
        return Out.str(); \
    }

struct StringArray
{
    const char* MessageHead = nullptr;
    unsigned int Index = 0;
    unsigned int MessageSize = 0;
};

template <typename T>
inline void PrettyPrint(std::stringstream& Out, const T& Value)
{
    if constexpr (std::is_same_v<T, std::string_view>)
    {
        Out << Value;
    }
    else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char[]>)
    {
        if (Value == nullptr)
        {
            Out << "<null>";
        }
        else
        {
            Out << Value;
        }
    }
    else if constexpr (std::is_same_v<T, GUID>)
    {
        Out << wsl::shared::string::GuidToString<char>(Value);
    }
    else if constexpr (std::is_same_v<T, char>)
    {
        Out << Value;
    }
    else if constexpr (std::is_fundamental_v<T> || std::is_enum_v<T>)
    {
        // N.B. Enum can be specialized by creating an overload for this method.
        Out << std::to_string(Value);
    }
    else if constexpr (std::is_same_v<T, StringArray>)
    {
        if (Value.Index <= 0)
        {
            Out << "<empty>";
            return;
        }

        gsl::span<const char> span(Value.MessageHead + Value.Index, Value.MessageHead + Value.MessageSize);
        Out << wsl::shared::string::Join(wsl::shared::string::ArrayFromSpan(gsl::as_bytes(span)), ',');
    }
    else
    {
        Out << "{";
        Value.PrettyPrintImpl(Out);
        Out << "}";
    }
}

template <typename T, int Size>
inline void PrettyPrint(std::stringstream& Out, const T (&Value)[Size])
{
    Out << "[";
    for (auto i = 0; i < Size; i++)
    {
        if (i > 0 && i < Size)
        {
            Out << ",";
        }

        PrettyPrint(Out, Value[i]);
    }

    Out << "]";
}

template <typename TArg>
inline void PrettyPrintField(std::stringstream& Out, const char* FieldName, const TArg& FieldValue)
{
    Out << FieldName << " = ";
    PrettyPrint(Out, FieldValue);
    Out << "\n";
}

template <typename TFirst, typename... TArgs>
inline void PrettyPrintField(std::stringstream& Out, const char* FieldName, const TFirst& FieldValue, TArgs&&... Args)
{
    PrettyPrintField(Out, FieldName, FieldValue);
    PrettyPrintField(Out, std::forward<TArgs>(Args)...);
}