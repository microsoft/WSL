/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    CommandLine.h

Abstract:

    This file contains the command line parsing logic.

--*/

#pragma once

#include <string>
#include <functional>
#include <type_traits>
#include "Localization.h"

namespace wsl::shared {

#ifdef WIN32

#define THROW_USER_ERROR(Message) THROW_HR_WITH_USER_ERROR(E_INVALIDARG, (Message))
using TChar = wchar_t;

#else

using TChar = char;

#endif

using TString = std::basic_string<TChar>;

using TMatchMethod = std::function<bool(const TChar*, int)>;
using TParseMethod = std::function<int(const TChar*)>;

struct Argument
{
    TMatchMethod Matches;
    TParseMethod Consume;
    bool Positional;
};

template <typename T, T Flag>
struct SetFlag
{
    T& value;

    void operator()() const
    {
        WI_SetFlag(value, Flag);
    }
};

template <typename T>
struct is_optional : std::false_type
{
};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type
{
};

template <typename T>
struct Integer
{
    T& value;

    int operator()(const TChar* input) const
    {
        if (input == nullptr)
        {
            return -1;
        }

        if constexpr (is_optional<T>::value)
        {
            value.emplace();
            return Integer<typename T::value_type>{value.value()}(input);
        }
        else
        {

#ifdef WIN32

            const auto utf8value = wsl::shared::string::WideToMultiByte(input);
            auto result = std::from_chars(utf8value.c_str(), utf8value.c_str() + utf8value.size(), value);

#else
            std::from_chars_result result{};
            if constexpr (std::is_enum_v<T>)
            {
                result = std::from_chars(input, input + strlen(input), reinterpret_cast<std::underlying_type_t<T>&>(value));
            }
            else
            {
                result = std::from_chars(input, input + strlen(input), value);
            }

#endif

            if (result.ec == std::errc::invalid_argument)
            {
                THROW_USER_ERROR(wsl::shared::Localization::MessageInvalidInteger(input));
            }
        }

        return 1;
    }
};

#ifdef WIN32
template <typename T>
struct UnquotedPath
{
    T& value;

    int operator()(const TChar* input) const
    {
        if (input == nullptr)
        {
            return -1;
        }

        value = wsl::windows::common::filesystem::UnquotePath(input);

        return 1;
    }
};

template <typename T>
struct AbsolutePath
{
    T& value;

    int operator()(const TChar* input) const
    {
        if (input == nullptr)
        {
            return -1;
        }

        if (PathIsRelative(input))
        {
            std::error_code error;
            value = std::filesystem::absolute(input, error);
            if (error)
            {
                THROW_WIN32(error.value());
            }
        }
        else
        {
            value = input;
        }

        return 1;
    }
};

struct Handle
{
    wil::unique_handle& output;

    int operator()(const TChar* input) const
    {
        if (input == nullptr)
        {
            return -1;
        }

        output.reset(ULongToHandle(wcstoul(input, nullptr, 0)));

        return 1;
    }
};

#else

struct UniqueFd
{
    wil::unique_fd& output;

    int operator()(const TChar* input)
    {
        if (input == nullptr)
        {
            return -1;
        }

        int fd = -1;
        auto result = std::from_chars(input, input + strlen(input), fd);
        if (result.ec == std::errc::invalid_argument || fd < 0)
        {
            THROW_USER_ERROR(wsl::shared::Localization::MessageInvalidInteger(input));
        }

        output.reset(fd);

        return 1;
    }
};

#endif

template <typename T>
struct ParsedBool
{
    T& value;

    int operator()(const TChar* input) const
    {
        if (input == nullptr)
        {
            return -1;
        }

        auto result = wsl::shared::string::ParseBool(input);
        if (!result.has_value())
        {
            THROW_USER_ERROR(wsl::shared::Localization::MessageInvalidBoolean(input));
        }

        value = result.value();
        return 1;
    }
};

template <typename T>
struct SizeString
{
    T& value;

    int operator()(const TChar* input) const
    {
        if (input == nullptr)
        {
            return -1;
        }

        auto parsed = wsl::shared::string::ParseMemorySize(input);
        if (!parsed.has_value())
        {
            THROW_USER_ERROR(wsl::shared::Localization::MessageInvalidSize(input));
        }

        value = parsed;
        return 1;
    }
};

struct NoOp
{
    void operator()(const TChar*) const
    {
    }
};

template <typename T, T SetValue>
struct UniqueSetValue
{
    std::optional<T>& value;
    std::function<TString()> errorMessage;

    void operator()()
    {
        if (value.has_value())
        {
            THROW_USER_ERROR(errorMessage());
        }

        value = SetValue;
    }
};

class ArgumentParser
{
public:
#ifdef WIN32

    ArgumentParser(const std::wstring& CommandLine, LPCWSTR Name, int StartIndex = 1) : m_startIndex(StartIndex), m_name(Name)
    {
        m_argv.reset(CommandLineToArgvW(std::wstring(CommandLine).c_str(), &m_argc));
        THROW_LAST_ERROR_IF(!m_argv);
    }

#else

    ArgumentParser(int argc, const char* const* argv) : m_argc(argc), m_argv(argv), m_startIndex(1)
    {
    }

#endif

    template <typename T>
    void AddArgument(T&& Output, const TChar* LongName, TChar ShortName = '\0')
    {
        auto match = [LongName, ShortName](const TChar* Name, int Pos) {
            if (Name == nullptr)
            {
                return false;
            }
            else if (LongName != nullptr && wsl::shared::string::IsEqual(Name, LongName))
            {
                return true;
            }
            else
            {
                return ShortName != '\0' && Name[0] == '-' && Name[1] == ShortName && Name[2] == '\0';
            }
        };

        m_arguments.emplace_back(std::move(match), BuildParseMethod(std::forward<T>(Output)), false);
    }

    template <typename T>
    void AddPositionalArgument(T&& Output, int Position)
    {
        WI_ASSERT(Position >= 0);

        auto match = [Position](const TChar*, int Index) { return Position == Index; };

        m_arguments.emplace_back(std::move(match), BuildParseMethod(std::forward<T>(Output)), true);
    }

    void Parse() const
    {
        int argumentPosition = 0;
        bool stopParameters = false;
        for (size_t i = m_startIndex; i < m_argc; i++)
        {
            if (!stopParameters && wsl::shared::string::IsEqual(m_argv[i], TEXT("--")))
            {
                stopParameters = true;
                continue;
            }

            bool foundMatch = false;
            int offset = 0;

            // Special case for short argument with multiple values like -abc
            if (!stopParameters && m_argv[i][0] == '-' && m_argv[i][1] != '-' && m_argv[i][1] != '\0' && m_argv[i][2] != '\0')
            {
                for (const auto* arg = &m_argv[i][1]; *arg != '\0'; arg++)
                {
                    foundMatch = false;
                    for (const auto& e : m_arguments)
                    {
                        const TChar fullArgument[] = {'-', *arg, '\0'};
                        if (!e.Positional && e.Matches(fullArgument, -1))
                        {
                            e.Consume(nullptr);
                            foundMatch = true;
                            break;
                        }
                    }

                    if (!foundMatch)
                    {
                        break;
                    }
                }
            }

            if (!foundMatch)
            {
                for (const auto& e : m_arguments)
                {
                    if (e.Matches(stopParameters ? nullptr : m_argv[i], m_argv[i][0] == '-' && m_argv[i][1] != '\0' && !stopParameters ? -1 : argumentPosition))
                    {
                        const TChar* value = nullptr;
                        if (e.Positional)
                        {
                            value = m_argv[i]; // Positional arguments directly receive arvg[i]
                        }
                        else if (i + 1 < m_argc)
                        {
                            value = m_argv[i + 1];
                        }

                        offset = e.Consume(value);
                        if (offset < 0)
                        {
                            WI_ASSERT(value == nullptr);
                            THROW_USER_ERROR(wsl::shared::Localization::MessageMissingArgument(m_argv[i], m_name ? m_name : m_argv[0]));
                        }

                        if (e.Positional) // Positional arguments can't consume extra arguments.
                        {
                            offset = 0;
                        }

                        i += offset;
                        foundMatch = true;

                        break;
                    }
                }
            }

            if (!foundMatch)
            {
                THROW_USER_ERROR(wsl::shared::Localization::MessageInvalidCommandLine(m_argv[i], m_name ? m_name : m_argv[0]));
            }

            if (i < m_argc && m_argv[i - offset][0] != '-')
            {
                argumentPosition++;
            }
        }
    }

private:
    template <typename T>
    static std::function<int(const TChar*)> BuildParseMethod(T&& Output)
    {
        if constexpr (std::is_rvalue_reference_v<T&&>)
        {
            return [Output = std::move(Output)](const TChar* Value) mutable { return ParseArgumentImpl<T>(Output, Value); };
        }
        else
        {
            return [&Output](const TChar* Value) mutable { return ParseArgumentImpl<T>(Output, Value); };
        }
    }

    template <typename T>
    static int ParseArgumentImpl(T& Output, const TChar* Value)
    {
        if constexpr (std::is_invocable_v<T, const TChar*>) // Callable with const TChar*.
        {
            if constexpr (std::is_same_v<std::invoke_result_t<T, const TChar*>, void>)
            {
                Output(Value);
                return 0;
            }
            else
            {
                return Output(Value);
            }
        }
        else if constexpr (std::is_invocable_v<T>) // Callable without argument.
        {
            Output();
            return 0;
        }
        else // Simple type.
        {
            return ParseSimpleArgument<std::remove_cvref_t<T>>(Output, Value);
        }
    }

    template <typename T>
    static int ParseSimpleArgument(T& Output, const TChar* Value)
    {
        // If this is a flag, just set the flag and exit
        if constexpr (std::is_same_v<T, bool>)
        {
            Output = true;
            return 0;
        }
        else
        {
            // Otherwise, we need an actual value
            if (Value == nullptr)
            {
                return -1;
            }

            if constexpr (std::is_same_v<T, GUID> || std::is_same_v<T, std::optional<GUID>>)
            {
                auto guid = wsl::shared::string::ToGuid(Value);
                if (!guid.has_value())
                {
                    THROW_USER_ERROR(wsl::shared::Localization::MessageInvalidGuid(Value));
                }

                Output = guid.value();
            }
            else
            {

                // If this assert is hit, an unsupported type was passed.
                static_assert(
                    std::is_same_v<T, TString> || std::is_same_v<T, std::optional<TString>> ||
                    std::is_same_v<T, std::filesystem::path> || std::is_same_v<T, const TChar*>);

                Output = Value;
            }
            return 1;
        }
    }

    std::vector<Argument> m_arguments;
    int m_argc{};

#ifdef WIN32

    wil::unique_hlocal_ptr<LPWSTR[]> m_argv;

#else

    const char* const* m_argv{};

#endif

    int m_startIndex{};
    const TChar* m_name{};
};
} // namespace wsl::shared

#ifdef WIN32
#undef THROW_USER_ERROR
#endif