// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeEnvironment.h"

using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

namespace {

    bool IsVariableStart(char value)
    {
        return value == '_' || (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    }

    bool IsVariableCharacter(char value)
    {
        return IsVariableStart(value) || (value >= '0' && value <= '9');
    }

    // Whitespace within a line; line breaks separate environment file entries.
    bool IsInlineSpace(char value)
    {
        return value == ' ' || value == '\t' || value == '\v' || value == '\f' || value == '\r';
    }

    bool IsEnvironmentKeyCharacter(char value)
    {
        return std::isalnum(static_cast<unsigned char>(value)) || static_cast<unsigned char>(value) >= 0x80 || value == '_' ||
               value == '.' || value == '-' || value == '[' || value == ']';
    }

    [[noreturn]] void ThrowInvalidReference(const std::filesystem::path& sourcePath, std::string_view value)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcComposeVariableInvalid(sourcePath.wstring(), std::string{value}));
    }

    [[noreturn]] void ThrowMissingVariable(const std::filesystem::path& sourcePath, std::string_view name, const std::string& reason)
    {
        if (reason.empty())
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcComposeVariableRequired(sourcePath.wstring(), std::string{name}));
        }

        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, Localization::MessageWslcComposeVariableRequiredReason(sourcePath.wstring(), std::string{name}, reason));
    }

    std::optional<size_t> FindClosingBrace(std::string_view value, size_t openIndex)
    {
        size_t depth = 0;
        for (size_t index = openIndex; index < value.size(); ++index)
        {
            if (value[index] == '{')
            {
                ++depth;
            }
            else if (value[index] == '}' && --depth == 0)
            {
                return index;
            }
        }

        return std::nullopt;
    }

    // Resolves the body of a ${...} reference: a name, optionally followed by :-, -, :+, +, :?, or ? and an argument.
    std::string SubstituteBraced(std::string_view body, std::string_view value, const ComposeVariableLookup& lookup, const std::filesystem::path& sourcePath)
    {
        if (body.empty() || !IsVariableStart(body.front()))
        {
            ThrowInvalidReference(sourcePath, value);
        }

        const auto nameEnd = static_cast<size_t>(std::find_if_not(body.begin() + 1, body.end(), IsVariableCharacter) - body.begin());
        const auto name = body.substr(0, nameEnd);
        auto modifier = body.substr(nameEnd);
        const auto variable = lookup(name);
        if (modifier.empty())
        {
            return variable.value_or(std::string{});
        }

        // The colon forms treat an empty value the same as an unset variable.
        const bool checkEmpty = modifier.starts_with(':');
        if (checkEmpty)
        {
            modifier.remove_prefix(1);
        }

        if (modifier.empty())
        {
            ThrowInvalidReference(sourcePath, value);
        }

        const bool set = variable.has_value() && (!checkEmpty || !variable->empty());
        const auto argument = modifier.substr(1);
        switch (modifier.front())
        {
        case '-':
            return set ? *variable : InterpolateComposeValue(argument, lookup, sourcePath);
        case '+':
            return set ? InterpolateComposeValue(argument, lookup, sourcePath) : variable.value_or(std::string{});
        case '?':
            if (!set)
            {
                ThrowMissingVariable(sourcePath, name, InterpolateComposeValue(argument, lookup, sourcePath));
            }

            return *variable;
        default:
            ThrowInvalidReference(sourcePath, value);
        }
    }

    // Expands escape sequences in a double-quoted environment file value. \$ becomes $$ so interpolation keeps a literal $.
    std::string ExpandEscapes(std::string_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (size_t index = 0; index < value.size(); ++index)
        {
            if (value[index] != '\\' || index + 1 == value.size())
            {
                result.push_back(value[index]);
                continue;
            }

            char replacement{};
            switch (value[index + 1])
            {
            case 'a':
                replacement = '\a';
                break;
            case 'b':
                replacement = '\b';
                break;
            case 'f':
                replacement = '\f';
                break;
            case 'n':
                replacement = '\n';
                break;
            case 'r':
                replacement = '\r';
                break;
            case 't':
                replacement = '\t';
                break;
            case 'v':
                replacement = '\v';
                break;
            case '"':
                replacement = '"';
                break;
            case '\\':
                replacement = '\\';
                break;
            case '$':
                result.append("$$");
                ++index;
                continue;
            default:
                result.push_back('\\');
                continue;
            }

            result.push_back(replacement);
            ++index;
        }

        return result;
    }

    void ParseEnvironmentFile(
        const ComposeEnvironmentFile& file, const ComposeVariableLookup& lookup, std::map<std::string, std::string, std::less<>>& variables)
    {
        const std::string_view content = file.Content;
        const auto fail = [&](size_t position) {
            const auto line = 1 + std::count(content.begin(), content.begin() + position, '\n');
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcComposeEnvFileInvalid(file.SourcePath.wstring(), line));
        };

        size_t index = 0;
        if (content.starts_with("\xEF\xBB\xBF"))
        {
            index = 3;
        }

        while (true)
        {
            while (index < content.size() && std::isspace(static_cast<unsigned char>(content[index])))
            {
                ++index;
            }

            if (index == content.size())
            {
                break;
            }

            if (content[index] == '#')
            {
                index = content.find('\n', index);
                if (index == std::string_view::npos)
                {
                    break;
                }

                continue;
            }

            const auto statement = index;
            constexpr std::string_view c_export{"export"};
            if (content.substr(index).starts_with(c_export) && index + c_export.size() < content.size() &&
                IsInlineSpace(content[index + c_export.size()]))
            {
                index += c_export.size();
                while (index < content.size() && IsInlineSpace(content[index]))
                {
                    ++index;
                }
            }

            // Keys end at '=', ':', or a line break; whitespace before the terminator is trimmed.
            const auto keyStart = index;
            std::optional<char> terminator;
            for (; index < content.size(); ++index)
            {
                const auto value = content[index];
                if (value == '=' || value == ':' || value == '\n')
                {
                    terminator = value;
                    break;
                }

                if (!IsInlineSpace(value) && !IsEnvironmentKeyCharacter(value))
                {
                    fail(index);
                }
            }

            auto key = content.substr(keyStart, index - keyStart);
            while (!key.empty() && IsInlineSpace(key.back()))
            {
                key.remove_suffix(1);
            }

            if (key.empty() || std::ranges::any_of(key, IsInlineSpace))
            {
                fail(statement);
            }

            // A name without a value inherits the variable when it is set.
            if (!terminator.has_value() || *terminator == '\n')
            {
                if (auto value = lookup(key))
                {
                    variables.insert_or_assign(std::string{key}, std::move(*value));
                }

                index = terminator.has_value() ? index + 1 : content.size();
                continue;
            }

            ++index;
            while (index < content.size() && IsInlineSpace(content[index]))
            {
                ++index;
            }

            std::string value;
            if (index < content.size() && (content[index] == '"' || content[index] == '\''))
            {
                // Quoted values may span lines; a backslash escapes the closing quote.
                const auto quote = content[index];
                bool escaped = false;
                bool closed = false;
                for (++index; index < content.size(); ++index)
                {
                    const auto character = content[index];
                    if (character == quote && !escaped)
                    {
                        closed = true;
                        ++index;
                        break;
                    }

                    if (character == '\\' && !escaped)
                    {
                        escaped = true;
                        continue;
                    }

                    if (escaped && character != quote)
                    {
                        value.push_back('\\');
                    }

                    escaped = false;
                    value.push_back(character);
                }

                if (!closed)
                {
                    fail(statement);
                }

                // Single-quoted values are literal.
                if (quote == '"')
                {
                    value = InterpolateComposeValue(ExpandEscapes(value), lookup, file.SourcePath);
                }
            }
            else
            {
                const auto lineEnd = content.find('\n', index);
                auto raw = content.substr(index, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - index);
                raw = raw.substr(0, raw.find(" #"));
                while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())))
                {
                    raw.remove_suffix(1);
                }

                value = InterpolateComposeValue(raw, lookup, file.SourcePath);
                index = lineEnd == std::string_view::npos ? content.size() : lineEnd + 1;
            }

            variables.insert_or_assign(std::string{key}, std::move(value));
        }
    }

} // namespace

std::string InterpolateComposeValue(std::string_view value, const ComposeVariableLookup& lookup, const std::filesystem::path& sourcePath)
{
    std::string result;
    size_t index = 0;
    while (index < value.size())
    {
        const auto dollar = value.find('$', index);
        if (dollar == std::string_view::npos)
        {
            result.append(value.substr(index));
            break;
        }

        result.append(value.substr(index, dollar - index));
        const auto next = dollar + 1 < value.size() ? value[dollar + 1] : '\0';
        if (next == '$')
        {
            result.push_back('$');
            index = dollar + 2;
            continue;
        }

        if (next == '{')
        {
            const auto close = FindClosingBrace(value, dollar + 1);
            if (!close.has_value())
            {
                ThrowInvalidReference(sourcePath, value);
            }

            result.append(SubstituteBraced(value.substr(dollar + 2, *close - dollar - 2), value, lookup, sourcePath));
            index = *close + 1;
            continue;
        }

        // A $ that does not start a reference is literal.
        if (!IsVariableStart(next))
        {
            result.push_back('$');
            index = dollar + 1;
            continue;
        }

        const auto nameEnd =
            static_cast<size_t>(std::find_if_not(value.begin() + dollar + 1, value.end(), IsVariableCharacter) - value.begin());
        result.append(lookup(value.substr(dollar + 1, nameEnd - dollar - 1)).value_or(std::string{}));
        index = nameEnd;
    }

    return result;
}

ComposeVariables::ComposeVariables(const std::vector<std::string>& processEnvironment, const std::vector<ComposeEnvironmentFile>& files)
{
    std::map<std::string, std::string, std::less<>> process;
    for (const auto& entry : processEnvironment)
    {
        const auto separator = entry.find('=');
        if (separator == std::string::npos || separator == 0)
        {
            continue;
        }

        process.insert_or_assign(entry.substr(0, separator), entry.substr(separator + 1));
    }

    // References inside environment files see the process environment first, then values already read.
    const ComposeVariableLookup lookup = [&](std::string_view name) -> std::optional<std::string> {
        if (const auto found = process.find(name); found != process.end())
        {
            return found->second;
        }

        if (const auto found = m_variables.find(name); found != m_variables.end())
        {
            return found->second;
        }

        return std::nullopt;
    };

    for (const auto& file : files)
    {
        ParseEnvironmentFile(file, lookup, m_variables);
    }

    for (auto& [name, value] : process)
    {
        m_variables.insert_or_assign(name, std::move(value));
    }
}

std::optional<std::string> ComposeVariables::Find(std::string_view name) const
{
    const auto found = m_variables.find(name);
    if (found == m_variables.end())
    {
        return std::nullopt;
    }

    return found->second;
}

std::string ComposeVariables::Interpolate(std::string_view value, const std::filesystem::path& sourcePath) const
{
    return InterpolateComposeValue(value, [this](std::string_view name) { return Find(name); }, sourcePath);
}

} // namespace wsl::windows::service::wslc
