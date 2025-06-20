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

#include "defs.h"
#include "stringshared.h"

#ifdef WIN32
#include <guiddef.h>
#else
#include "lxdef.h"
#endif

#define FIELD(Name) #Name, Name

#define STRING_FIELD(Name) #Name, (Name <= 0 ? "<empty>" : ((char*)(this)) + Name)

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

template <typename T>
inline void PrettyPrint(std::stringstream& Out, const T& Value)
{
    if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char[]>)
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
        if (i > 0 && i < Size - 1)
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