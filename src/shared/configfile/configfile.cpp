/*++

Copyright (c) Microsoft Corporation. All rights reserved

Parses .gitconfig-style properties files. This consists of key-value pairs
divided into sections.

For example:

[section1]
key1 = value
key2 = " value with leading and trailing spaces "
key3 = value with \"embedded quotes\"
boolkey = true

# Comments start with hash
[section2]
intkey = 37
intkey2 = 0x3000    # integers can be in hex
intkey3 = 0644      # octal is OK too

[section3]
key = this key has a line continuation \
    so that it can wrap to the next line

key2 = this key has an \n embedded newline
key3 = "this key uses quotes to # include a comment prefix"

--*/

#if defined(_MSC_VER)

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strdup _strdup
#define _WINSOCKAPI_

#include "precomp.h"

using wsl::shared::string::MacAddress;

#else

#include <cassert>
#include <csignal>

#endif

#include "configfile.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <format>
#include "stringshared.h"
#include "Localization.h"

using wsl::shared::Localization;

bool ConfigKey::ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, bool& result)
{
    const auto parsed = wsl::shared::string::ParseBool(value);
    if (!parsed.has_value())
    {
        EMIT_USER_WARNING(Localization::MessageConfigInvalidBoolean(value, name, filePath, fileLine));
        return false;
    }

    result = parsed.value();
    return true;
}

bool ConfigKey::ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, int& result)
{
    char* end{};
    const long number = strtol(value, &end, 0);
    if (*value == '\0' || *end != '\0' || number < INT_MIN || number > INT_MAX)
    {
        EMIT_USER_WARNING(Localization::MessageConfigInvalidInteger(value, name, filePath, fileLine));
        return false;
    }

    result = number;
    return true;
}

bool ConfigKey::ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, std::string& result)
{
    result = value;
    return true;
}

bool ConfigKey::ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, MemoryString result)
{
    const auto memory = wsl::shared::string::ParseMemorySize(value);
    if (!memory.has_value())
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageInvalidNumberString(value, name, filePath, fileLine));
        return false;
    }

    result.m_value = memory.value();
    return true;
}

bool ConfigKey::ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, std::wstring& result)
{
    result = wsl::shared::string::MultiByteToWide(value);
    return true;
}

#ifdef WIN32

bool ConfigKey::ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, MacAddress& outValue)
{
    if (auto parsed = wsl::shared::string::ParseMacAddressNoThrow<char>(value))
    {
        outValue = std::move(parsed.value());
    }
    else
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageConfigMacAddress(value, name, filePath, fileLine));
    }

    return true;
}

#endif

bool ConfigKey::ParseImpl(const char* name, const char* value, const wchar_t* filePath, unsigned long fileLine, std::filesystem::path& result)
{
    result = wsl::shared::string::MultiByteToWide(value);
    return true;
}

std::wstring ConfigKey::GetValueImpl(bool result)
{
    return result ? L"true" : L"false";
}

std::wstring ConfigKey::GetValueImpl(int result)
{
    return std::to_wstring(result);
}

std::wstring ConfigKey::GetValueImpl(const std::string& result)
{
    return wsl::shared::string::MultiByteToWide(result);
}

std::wstring ConfigKey::GetValueImpl(const std::optional<std::string>& result)
{
    return result.has_value() ? wsl::shared::string::MultiByteToWide(result.value()) : L"";
}

std::wstring ConfigKey::GetValueImpl(const MemoryString& result)
{
    return std::to_wstring(result.m_value);
}

std::wstring ConfigKey::GetValueImpl(const std::wstring& result)
{
    return result;
}

#ifdef WIN32

std::wstring ConfigKey::GetValueImpl(const MacAddress& result)
{
    return wsl::shared::string::FormatMacAddress(result, L':');
}

#endif

bool ConfigKey::Matches(const char* name) const
{
    return std::any_of(m_names.begin(), m_names.end(), [&](const auto& e) { return strcasecmp(e, name) == 0; });
}

bool ConfigKey::Matches(const char* name, size_t length) const
{
    return std::any_of(m_names.begin(), m_names.end(), [&](const auto& e) { return strncasecmp(e, name, length) == 0; });
}

void ConfigKey::Parse(const char* name, const char* value, const wchar_t* fileName, unsigned long line)
{
    if (m_parseResult.has_value())
    {
        EMIT_USER_WARNING(
            Localization::MessageConfigKeyDuplicated(name, fileName, line, m_parseResult->first, fileName, m_parseResult->second));
        return;
    }

    m_parse(name, value, fileName, line);
    m_parseResult.emplace(name, line);
}

const std::vector<const char*>& ConfigKey::GetNames() const
{
    return m_names;
}

std::wstring ConfigKey::GetValue() const
{
    return m_getValue();
}

// Updates the configuration with the given value.
static void SetConfig(std::vector<ConfigKey>& keys, const char* keyName, const char* value, bool debug, const wchar_t* filePath, unsigned long fileLine)
{
    const auto key = std::find_if(keys.begin(), keys.end(), [keyName](const auto& e) { return e.Matches(keyName); });
    if (key == keys.end())
    {
        EMIT_USER_WARNING(Localization::MessageConfigUnknownKey(keyName, filePath, fileLine));
        return;
    }

    key->Parse(keyName, value, filePath, fileLine);
}

// Returns whether a character is a horizontal space (' ' or '\t').
static bool IsHSpace(wint_t ch)
{
    return ch == ' ' || ch == '\t';
}

// Parses a configuration file. If file is NULL, then just set the configuration
// to the default values.
int ParseConfigFile(std::vector<ConfigKey>& keys, FILE* file, int flags, const wchar_t* filePath)
{
    std::wstring emptyStr;
    return ParseConfigFile(keys, file, flags, filePath, emptyStr);
}

// Parses a configuration file. If file is NULL, then just set the configuration
// to the default values.
int ParseConfigFile(std::vector<ConfigKey>& keys, FILE* file, int flags, const wchar_t* filePath, std::wstring& configFileOutput, std::optional<ConfigKey> outputKey, bool removeKey)
{
    wint_t ch = 0;
    unsigned long line = 0;
    bool trailingComment = false;
    bool inQuote = false;
    size_t trimmedLength = 0;
    int result;
    size_t sectionLength = 0;
    std::string key = {0};
    std::string value = {0};

    // Function default is parse mode (updateConfigFile = false).
    // Otherwise, update mode (though parsing logic is still used).
    bool updateConfigFile = false;
    bool outputKeyValueUpdated = false;
    bool matchedKey = false;
    bool firstMatchedKey = false;

    if (outputKey.has_value())
    {
        updateConfigFile = true;
    }

    if (file == NULL)
    {
        result = 0;
        if (updateConfigFile && !outputKeyValueUpdated && !removeKey)
        {
            goto WriteNewKeyValue;
        }
        else
        {
            goto Done;
        }
    }

NewLine:
    if (!trailingComment)
    {
        line++;
    }

    // parse [section], key = value, or empty line
    for (;;)
    {
        if (updateConfigFile && ch != 0 && ch != WEOF)
        {
            if (trailingComment && matchedKey)
            {
                // If we're removing a key and have a trailing comment,
                // the comment will be preserved. The newline char will have
                // been removed from the output stream (due to key removal),
                // so insert it back here.
                configFileOutput += L'\n';
            }

            // Write the current character to output now, since, in
            // addition to writing the characters read in this loop,
            // future parsing may jump back to the NewLine label
            // and we assume the 'ch' has yet to be written.
            configFileOutput += ch;
        }

        // Skip any pending comment.
        if (ch == '#')
        {
            do
            {
                ch = fgetwc(file);

                if (updateConfigFile && ch != WEOF)
                {
                    // Write out the rest of the comment line.
                    configFileOutput += ch;
                }

                if (ch == '\r')
                {
                    ch = fgetwc(file);
                }

                if (ch == '\n')
                {
                    line++;
                }

            } while (ch != '\n' && ch != WEOF);

            if (trailingComment)
            {
                trailingComment = false;
            }
        }

        if (feof(file))
        {
            result = 0;
            if (updateConfigFile && !outputKeyValueUpdated && !removeKey)
            {
                goto WriteNewKeyValue;
            }
            else
            {
                goto Done;
            }
        }

        if (ferror(file))
        {
            result = -1;
            goto Done;
        }

        // Skip leading spaces.
        while (IsHSpace(ch = fgetwc(file)))
        {
            if (updateConfigFile)
            {
                configFileOutput += ch;
            }
        }

        switch (ch)
        {
        case WEOF:
            break;

        case '\r':
        {
            auto nextc = fgetwc(file);
            if (nextc == '\n')
            {
                line++;
            }
            else
            {
                ungetwc(nextc, file);
            }

            break;
        }

        case '\n':
            line++;
            break;

        case '#':
            break;

        case '[':
            // We're about to parse a new section. If we have an unwritten key-value
            // and the current section matches, write it now before moving to the new section.
            if (updateConfigFile && !outputKeyValueUpdated && !removeKey && sectionLength > 0)
            {
                const auto& outputConfigKey = outputKey.value();
                if (outputConfigKey.Matches(key.c_str(), sectionLength))
                {
                    const auto& keyNames = outputConfigKey.GetNames();
                    // Config key without name.
                    FAIL_FAST_IF(keyNames.empty());
                    const auto keyNameUtf8 = keyNames.front();
                    const auto keyName = wsl::shared::string::MultiByteToWide(keyNameUtf8);
                    const auto sectionKeySeparatorPos = keyName.find('.');
                    // Config key without separated section/key name
                    FAIL_FAST_IF(sectionKeySeparatorPos == std::string_view::npos);
                    // Config key without section name
                    FAIL_FAST_IF(sectionKeySeparatorPos == 0);
                    // Config key without key name
                    FAIL_FAST_IF(sectionKeySeparatorPos == (keyName.length() - 1));

                    // Remove any trailing newlines before inserting the new key-value
                    while (!configFileOutput.empty() && configFileOutput.back() == L'\n')
                    {
                        configFileOutput.pop_back();
                    }

                    auto keyValue = std::format(L"\n{}={}\n\n", keyName.substr(sectionKeySeparatorPos + 1), outputKey.value().GetValue());
                    configFileOutput += keyValue;
                    outputKeyValueUpdated = true;
                }
            }
            goto ParseSection;

        default:
            if (!isalpha(ch))
            {
                if (flags & CFG_DEBUG)
                {
                    fputs("expected a-z\n", stderr);
                }

                EMIT_USER_WARNING(Localization::MessageConfigInvalidKey(filePath, line));

                if (updateConfigFile)
                {
                    // Always write out the invalid character
                    // prior to jumping to the InvalidLine label.
                    configFileOutput += ch;
                    ch = 0;
                }

                goto InvalidLine;
            }

            goto ParseKeyValue;
        }
    }

ParseSection:
    // parse [section] ([ is already parsed)
    if (updateConfigFile)
    {
        // Write the '[' character to the output.
        configFileOutput += ch;
    }

    ch = fgetwc(file);

    if (!isalpha(ch))
    {
        if (flags & CFG_DEBUG)
        {
            fputs("expected a-z\n", stderr);
        }

        EMIT_USER_WARNING(Localization::MessageConfigInvalidSection(filePath, line));

        if (updateConfigFile)
        {
            // Always write out the invalid character
            // prior to jumping to the InvalidLine label.
            configFileOutput += ch;
            ch = 0;
        }

        goto InvalidLine;
    }

    key.clear();

    do
    {
        if (updateConfigFile)
        {
            // Write the first alpha character of the section
            // name followed by the rest of the section name.
            configFileOutput += ch;
        }

        key += static_cast<char>(ch);

        ch = fgetwc(file);
    } while (isalnum(ch));

    if (ch != ']')
    {
        if (flags & CFG_DEBUG)
        {
            fputs("expected ]\n", stderr);
        }

        EMIT_USER_WARNING(Localization::MessageConfigExpected("']'", filePath, line));

        if (updateConfigFile)
        {
            // Always write out the invalid character
            // prior to jumping to the InvalidLine label.
            configFileOutput += ch;
            ch = 0;
        }

        goto InvalidLine;
    }

    if (updateConfigFile)
    {
        // Write the ']' character to the output.
        configFileOutput += ch;
    }

    // Skip trailing space.
    while (IsHSpace(ch = fgetwc(file)))
    {
        if (updateConfigFile)
        {
            configFileOutput += ch;
        }
    }

    switch (ch)
    {
    case WEOF:
    case '\n':
    case '\r':
        break;

    case '#':
        trailingComment = true;
        break;

    default:
        if (flags & CFG_DEBUG)
        {
            fputs("expected space or EOL\n", stderr);
        }

        EMIT_USER_WARNING(Localization::MessageConfigExpected("' ' or '\\n'", filePath, line));

        if (updateConfigFile)
        {
            // Always write out the invalid character
            // prior to jumping to the InvalidLine label.
            configFileOutput += ch;
            ch = 0;
        }

        goto InvalidLine;
    }

    sectionLength = key.size();

    goto NewLine;

ParseKeyValue:
    // parse key = value. The first character of the key is in ch.
    key.resize(sectionLength);
    if (key.size() > 0)
    {
        key += '.';
    }

    do
    {
        if (updateConfigFile)
        {
            // Write out the first character of the key to the
            // output followed by the rest of the key name.
            configFileOutput += ch;
        }

        key += static_cast<char>(ch);

        ch = fgetwc(file);
    } while (isalnum(ch));

    // Skip leading space.
    while (IsHSpace(ch))
    {
        if (updateConfigFile)
        {
            configFileOutput += ch;
        }

        ch = fgetwc(file);
    }

    if (ch != '=')
    {
        if (flags & CFG_DEBUG)
        {
            fputs("expected =\n", stderr);
        }

        EMIT_USER_WARNING(Localization::MessageConfigExpected("'='", filePath, line));

        if (updateConfigFile)
        {
            // Always write out the invalid character
            // prior to jumping to the InvalidLine label.
            configFileOutput += ch;
            ch = 0;
        }

        goto InvalidLine;
    }

    if (updateConfigFile)
    {
        // Write the '=' character to the output.
        configFileOutput += ch;
    }

    // Skip trailing space.
    while (IsHSpace(ch = fgetwc(file)))
    {
        if (updateConfigFile)
        {
            configFileOutput += ch;
        }
    }

    // Only match the first instance of the key in the input file.
    // In other words, if we've already updated the matched key value,
    // then ignore updating any other keys that match.
    // This is consistent with the behavior of the parsing logic.
    firstMatchedKey = false;
    if (updateConfigFile && !outputKeyValueUpdated && !removeKey)
    {
        firstMatchedKey = outputKey.value().Matches(key.c_str());
    }

    // There may be multiple instances of the same key in the input file,
    // so we need to find and remove all instances of the key.
    matchedKey = false;
    if (updateConfigFile && removeKey)
    {
        matchedKey = outputKey.value().Matches(key.c_str());
        if (matchedKey)
        {
            auto previousNewLine = configFileOutput.rfind(L'\n');
            if (previousNewLine != std::wstring::npos)
            {
                configFileOutput = configFileOutput.substr(0, previousNewLine);
            }
        }
    }

    // Parse the value by removing unescaped quotes, handling escaped n, t, \,
    // ", and NewLine (line continuation). End parsing on a NewLine, EOF, or
    // comment (#).
    value.clear();
    trimmedLength = 0;
    inQuote = false;
    while (ch != WEOF && ch != '\n' && ch != '\r')
    {
        if (updateConfigFile && !firstMatchedKey && !matchedKey && ch != '#')
        {
            // Write out the first character of the value to
            // the output followed by the rest of the value.
            // Don't write the '#' as it will be written by the
            // NewLine label after the ValueDone label. This is
            // done to ensure consistency with the writing logic.
            configFileOutput += ch;
        }

        switch (ch)
        {
        case '"':
            inQuote = !inQuote;
            break;

        case '\\':
        {
            auto ch2 = fgetwc(file);

            if (updateConfigFile && !firstMatchedKey && !matchedKey && ch2 != WEOF)
            {
                // Write out the escaped character to the output, also,
                // handling the case where ch2 is an invalid character.
                configFileOutput += ch2;
            }

            switch (ch2)
            {
            case '\\':
            case '"':
                value += static_cast<char>(ch2);

                break;

            case 'b':
                value += '\b';
                break;

            case 'n':
                value += '\n';
                break;

            case 't':
                value += '\t';
                break;

            case '\r':
                break;

            case '\n':
                // Line continuation. Skip both characters.
                line++;
                break;

            default:
                if (flags & CFG_DEBUG)
                {
                    fprintf(stderr, "unexpected escaped character %lc\n", ch2);
                }

                EMIT_USER_WARNING(Localization::MessageConfigInvalidEscape(static_cast<wchar_t>(ch2), filePath, line));

                if (firstMatchedKey)
                {
                    // This key value will be overwritten, so we can ignore any malformed values,
                    // since none of the value should/will have been written to the output file.
                    // However, we can still inform the user of the issue per the above warning.
                    break;
                }

                goto InvalidLine;
            }
        }

        break;

        case '#':
            if (!inQuote)
            {
                trailingComment = true;
                goto ValueDone;
            }
        default:
            value += static_cast<char>(ch);

            break;
        }

        // Track the length without trailing space.
        if (!IsHSpace(ch))
        {
            trimmedLength = value.size();
        }
        ch = fgetwc(file);
    }

ValueDone:
    // If we overwrote an existing key value, where the value is malformed, we can ignore it.
    if (inQuote)
    {
        if (flags & CFG_DEBUG)
        {
            fprintf(stderr, "expected \"\n");
        }

        EMIT_USER_WARNING(Localization::MessageConfigExpected("'", filePath, line));

        // This key value will be overwritten, so we can ignore any malformed values.
        // However, we can still inform the user of the issue per warning above.
        if (!firstMatchedKey || !matchedKey)
        {
            goto InvalidLine;
        }
    }

    if (firstMatchedKey)
    {
        for (auto outValueCh : outputKey.value().GetValue())
        {
            configFileOutput += outValueCh;
        }

        // Preserve any spacing in the parsed value.
        // Invalid values are still parsed in the case
        // of trailing comments.
        for (size_t spaceIdx = trimmedLength; spaceIdx < value.size(); spaceIdx++)
        {
            configFileOutput += value[spaceIdx];
        }

        outputKeyValueUpdated = true;
    }
    else if (!matchedKey)
    {
        // Trim any trailing space.
        value.resize(trimmedLength);
        SetConfig(keys, key.c_str(), value.c_str(), flags & CFG_DEBUG, filePath, line);
    }

    goto NewLine;

InvalidLine:
    if (!(flags & CFG_SKIP_INVALID_LINES))
    {
        result = -1;
        goto Done;
    }

    while (ch != WEOF && ch != '\n')
    {
        ch = fgetwc(file);

        if (updateConfigFile && ch != WEOF && ch != '\n' && ch != '\r')
        {
            // Write out the rest of the remaining
            // invalid line. WEOF and '\n' will be
            // handled/written by the NewLine label.
            configFileOutput += ch;
        }
    }

    goto NewLine;

WriteNewKeyValue:
{
    const auto& outputConfigKey = outputKey.value();
    const auto& keyNames = outputConfigKey.GetNames();
    // Config key without name.
    FAIL_FAST_IF(keyNames.empty());
    const auto keyNameUtf8 = keyNames.front();
    const auto keyName = wsl::shared::string::MultiByteToWide(keyNameUtf8);
    const auto sectionKeySeparatorPos = keyName.find('.');
    // Config key without separated section/key name
    FAIL_FAST_IF(sectionKeySeparatorPos == std::string_view::npos);
    // Config key without section name
    FAIL_FAST_IF(sectionKeySeparatorPos == 0);
    // Config key without key name
    FAIL_FAST_IF(sectionKeySeparatorPos == (keyName.length() - 1));

    // This is a new key/value pair not present in the input file, so write it out.
    // No need for newline if this is the first key/value pair.
    if (file != NULL)
    {
        configFileOutput += L'\n';
    }

    // Check if we currently parsed a key and the key matches the section name.
    // In this case, we don't need to write the section name again.
    if (!(sectionLength > 0 && outputConfigKey.Matches(key.c_str(), sectionLength)))
    {
        configFileOutput += std::format(L"[{}]\n", keyName.substr(0, sectionKeySeparatorPos));
    }

    configFileOutput += std::format(L"{}={}", keyName.substr(sectionKeySeparatorPos + 1), outputKey.value().GetValue());

    outputKeyValueUpdated = true;
    goto Done;
}

Done:
    return result;
}