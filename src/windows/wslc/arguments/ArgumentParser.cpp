/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentParser.cpp

Abstract:

    Implementation of the ArgumentParser class.

--*/
#include "ArgumentParser.h"
#include "Localization.h"

using namespace wsl::shared;

namespace wsl::windows::wslc {
namespace {
    [[noreturn]] void ThrowUnsupportedArgument(const Argument& argument)
    {
        WI_ASSERT(argument.IsOption());
        throw ArgumentException(Localization::WSLCCLI_UnsupportedOptionError(std::wstring(2, WSLC_CLI_ARG_ID_CHAR) + argument.Name()));
    }
} // namespace

ParseArgumentsStateMachine::ParseArgumentsStateMachine(
    Invocation& inv,
    ArgMap& execArgs,
    std::vector<Argument> arguments,
    bool optionsOnly,
    bool stopOnUnknown,
    const std::vector<Argument>& overridableDefaults,
    std::vector<ArgumentDeprecation> deprecations,
    std::vector<ArgType> unsupportedArguments) :
    m_invocation(inv),
    m_executionArgs(execArgs),
    m_arguments(std::move(arguments)),
    m_argumentDeprecations(std::move(deprecations)),
    m_invocationItr(m_invocation.begin()),
    m_optionsOnly(optionsOnly),
    m_stopOnUnknown(stopOnUnknown)
{
    for (const auto& arg : m_arguments)
    {
        switch (arg.Kind())
        {
        case Kind::Value:
            m_standardArgs.emplace_back(arg);
            break;
        case Kind::Flag:
            m_standardArgs.emplace_back(arg);
            break;
        case Kind::Positional:
            m_positionalArgs.emplace_back(arg);
            break;
        case Kind::Forward:
            m_forwardArgs.emplace_back(arg);
            break;
        }
    }

    for (const auto type : unsupportedArguments)
    {
        const auto argument = Argument::Create(type);
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            !m_unsupportedArguments.emplace(type).second,
            "Argument type %d is declared unsupported more than once",
            static_cast<int>(type));
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            FindArgument(type) != nullptr,
            "Unsupported argument type %d is also declared as a command argument",
            static_cast<int>(type));
        THROW_HR_IF_MSG(E_INVALIDARG, !argument.IsOption(), "Unsupported argument type %d is not an option", static_cast<int>(type));
        m_standardArgs.emplace_back(argument);
    }

    std::set<ArgType> deprecatedTypes;
    for (const auto& deprecation : m_argumentDeprecations)
    {
        const auto deprecatedType = deprecation.DeprecatedType();
        const auto replacementType = deprecation.ReplacementType();
        const auto deprecatedArgument = Argument::Create(deprecatedType);
        const auto replacementArgument = FindArgument(replacementType);

        THROW_HR_IF_MSG(
            E_INVALIDARG,
            !deprecatedTypes.emplace(deprecatedType).second,
            "Argument type %d has multiple deprecation mappings",
            static_cast<int>(deprecatedType));
        THROW_HR_IF_MSG(
            E_INVALIDARG, deprecatedType == replacementType, "Deprecated argument type %d cannot replace itself", static_cast<int>(deprecatedType));
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            FindArgument(deprecatedType) != nullptr,
            "Deprecated argument type %d is also declared as a command argument",
            static_cast<int>(deprecatedType));
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            m_unsupportedArguments.contains(deprecatedType),
            "Argument type %d is declared both deprecated and unsupported",
            static_cast<int>(deprecatedType));
        THROW_HR_IF_MSG(
            E_INVALIDARG, replacementArgument == nullptr, "Replacement argument type %d is not a command argument", static_cast<int>(replacementType));
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            !deprecatedArgument.IsOption() || deprecatedArgument.Kind() != replacementArgument->Kind(),
            "Deprecated argument type %d is incompatible with replacement type %d",
            static_cast<int>(deprecatedType),
            static_cast<int>(replacementType));
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            std::ranges::any_of(
                m_argumentDeprecations,
                [replacementType](const auto& candidate) { return candidate.DeprecatedType() == replacementType; }),
            "Deprecated argument type %d maps to another deprecated argument type",
            static_cast<int>(deprecatedType));

        m_standardArgs.emplace_back(deprecatedArgument);
        m_executionArgs.RegisterArgumentDeprecation(deprecatedType, replacementType);
    }

    m_positionalSearchItr = m_positionalArgs.begin();

    m_overridableDefaults.reserve(overridableDefaults.size());
    for (const auto& arg : overridableDefaults)
    {
        m_overridableDefaults.push_back(arg.Type());
    }
}

bool ParseArgumentsStateMachine::Step()
{
    if (m_stopped || m_invocationItr == m_invocation.end())
    {
        return false;
    }

    m_state = StepInternal();
    return true;
}

void ParseArgumentsStateMachine::ThrowIfError() const
{
    if (m_state.Exception())
    {
        throw m_state.Exception().value();
    }
    // If the next argument was to be a value, but none was provided, convert it to an exception.
    else if (m_state.Type() && m_invocationItr == m_invocation.end())
    {
        const auto* argument = FindArgument(m_state.Type().value());
        const auto message = Localization::WSLCCLI_MissingArgumentError(m_state.Arg());
        throw argument != nullptr ? ArgumentException(message, *argument) : ArgumentException(message);
    }
}

void ParseArgumentsStateMachine::AdvanceToNextPositional(std::vector<Argument>::iterator& itr) const
{
    // Skip positionals that are already full. A single-value positional is full once it
    // holds one value; an unlimited positional is never full.
    while (itr != m_positionalArgs.end() && itr->IsSingle() && m_executionArgs.Count(itr->Type()) >= 1)
    {
        ++itr;
    }
}

const Argument* ParseArgumentsStateMachine::NextPositional()
{
    AdvanceToNextPositional(m_positionalSearchItr);
    return m_positionalSearchItr != m_positionalArgs.end() ? &*m_positionalSearchItr : nullptr;
}

bool ParseArgumentsStateMachine::HasNextPositional() const
{
    auto itr = m_positionalSearchItr;
    AdvanceToNextPositional(itr);
    return itr != m_positionalArgs.end();
}

ParseArgumentsStateMachine::State ParseArgumentsStateMachine::BackUpAndStop()
{
    --m_invocationItr;
    m_stopped = true;
    return {};
}

bool ParseArgumentsStateMachine::ConsumeOverrideIfPresent(ArgType type)
{
    auto it = std::find(m_overridableDefaults.begin(), m_overridableDefaults.end(), type);
    if (it == m_overridableDefaults.end())
    {
        return false;
    }

    m_executionArgs.Remove(type);
    m_overridableDefaults.erase(it);
    return true;
}

void ParseArgumentsStateMachine::ClearArgument(ArgType type)
{
    // Drop any preloaded overridable default and remove previously parsed entries so the
    // argument is left absent. This is the single-value/last-wins primitive shared by
    // SetFlag (which then stores the flag's explicit value) and AddValue (single-value args).
    ConsumeOverrideIfPresent(type);
    m_executionArgs.Remove(type);
}

void ParseArgumentsStateMachine::SetFlag(ArgType type, bool value)
{
    // Boolean flags store their explicit parsed value (true or false) so a flag whose behavior
    // is on by default can be turned off with "--flag=false". Clearing first collapses CLI
    // duplicates to a single entry and gives docker's last-wins behavior for repeated flags
    // (e.g. "--flag --flag=false" ends up false). Read flags back via ArgMap::GetValue(defaultValue), which
    // folds the presence check and the stored value into one test, rather than a bare Contains().
    ClearArgument(type);
    m_executionArgs.Add(type, value);
}

std::wstring_view ParseArgumentsStateMachine::StripSurroundingQuotes(std::wstring_view value)
{
    if (value.length() >= 2 && value.front() == L'"' && value.back() == L'"')
    {
        value = value.substr(1, value.length() - 2);
    }

    return value;
}

ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ApplyFlagValue(ArgType type, std::wstring_view value, const std::wstring_view& currArg)
{
    const auto unquoted = StripSurroundingQuotes(value);
    const auto boolVal = string::ParseBool(std::wstring(unquoted).c_str(), /*AllowExtendedForms*/ true);
    if (!boolVal.has_value())
    {
        const auto* argument = FindArgument(type);
        const auto message = Localization::WSLCCLI_FlagInvalidBooleanError(currArg);
        return argument != nullptr ? ArgumentException(message, *argument) : ArgumentException(message);
    }

    SetFlag(type, boolVal.value());
    return {};
}

const Argument* ParseArgumentsStateMachine::FindArgument(ArgType type) const
{
    for (const auto& arg : m_arguments)
    {
        if (arg.Type() == type)
        {
            return &arg;
        }
    }

    return nullptr;
}

const ArgumentDeprecation* ParseArgumentsStateMachine::FindArgumentDeprecation(ArgType type) const
{
    const auto deprecation =
        std::ranges::find(m_argumentDeprecations, type, [](const auto& value) { return value.DeprecatedType(); });
    return deprecation != m_argumentDeprecations.end() ? &*deprecation : nullptr;
}

bool ParseArgumentsStateMachine::IsUnsupportedArgument(ArgType type) const
{
    return m_unsupportedArguments.contains(type);
}

ArgType ParseArgumentsStateMachine::ResolveArgumentType(const Argument& argument)
{
    const auto deprecation = FindArgumentDeprecation(argument.Type());
    if (deprecation == nullptr)
    {
        if (argument.IsSingle())
        {
            const auto deprecatedType = m_deprecatedReplacementTypesUsed.find(argument.Type());
            if (deprecatedType != m_deprecatedReplacementTypesUsed.end())
            {
                const auto deprecatedArgument = Argument::Create(deprecatedType->second);
                throw ArgumentException(
                    Localization::WSLCCLI_MultipleExclusiveArgumentsProvided(
                        std::wstring(2, WSLC_CLI_ARG_ID_CHAR) + deprecatedArgument.Name() + L", " +
                        std::wstring(2, WSLC_CLI_ARG_ID_CHAR) + argument.Name()),
                    argument);
            }
        }

        m_canonicalArgumentsUsed.emplace(argument.Type());
        return argument.Type();
    }

    const auto replacement = FindArgument(deprecation->ReplacementType());
    WI_ASSERT(replacement != nullptr);
    if (replacement != nullptr && replacement->IsSingle() && m_canonicalArgumentsUsed.contains(replacement->Type()))
    {
        throw ArgumentException(
            Localization::WSLCCLI_MultipleExclusiveArgumentsProvided(
                std::wstring(2, WSLC_CLI_ARG_ID_CHAR) + argument.Name() + L", " + std::wstring(2, WSLC_CLI_ARG_ID_CHAR) +
                replacement->Name()),
            *replacement);
    }

    m_deprecatedReplacementTypesUsed.emplace(deprecation->ReplacementType(), deprecation->DeprecatedType());
    m_executionArgs.RecordDeprecatedArgumentUse(deprecation->DeprecatedType());
    return deprecation->ReplacementType();
}

void ParseArgumentsStateMachine::AddValue(ArgType type, std::wstring value)
{
    const Argument* arg = FindArgument(type);
    WI_ASSERT(arg != nullptr);

    // Unlimited value args accumulate; single-value args are last-wins. In both cases the
    // first CLI value must displace a preloaded overridable default, which ClearArgument
    // (single) and ConsumeOverrideIfPresent (unlimited) each handle.
    if (arg != nullptr && arg->IsUnlimited())
    {
        ConsumeOverrideIfPresent(type);
    }
    else
    {
        ClearArgument(type);
    }

    m_executionArgs.Add(type, std::move(value));
}

// Parse rules:
//  1. Token starting with a single '-' is an alias (1-2 chars):
//     a. Value: '-a=VALUE' / '-ab=VALUE' / '-a VALUE' / '-ab VALUE'
//     b. Flag:  trailing chars are additional flags; fails if any is non-flag.
//  2. Token starting with '--' is the full name: '--arg=VALUE' or '--arg VALUE'.
//  3. Anything else is the next positional.
//  4. Once a positional is seen, everything after stays positional.
//  5. If only one positional is defined, everything after it is forwarded.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::StepInternal()
{
    auto currArg = std::wstring_view{*m_invocationItr};
    ++m_invocationItr;

    // Pending value from the previous token.
    if (m_state.Type())
    {
        AddValue(m_state.Type().value(), std::wstring{currArg});
        return {};
    }

    // Anchored: remaining tokens are positional or forwarded.
    if (!m_forwardArgs.empty() && m_anchorPositional.has_value())
    {
        return ProcessAnchoredPositionals(currArg);
    }

    // Arg does not begin with '-' so it is neither an alias nor a named value, must be positional.
    if (currArg.empty() || currArg[0] != WSLC_CLI_ARG_ID_CHAR)
    {
        if (m_optionsOnly)
        {
            // Options-only mode: stop cleanly at the first positional token without
            // consuming it so the caller can resume parsing (e.g. subcommand resolution).
            return BackUpAndStop();
        }

        return ProcessPositionalArgument(currArg);
    }

    // The currentArg is non-empty, and starts with a -.
    if (currArg.length() == 1)
    {
        if (HasNextPositional())
        {
            // The '-' character may be a valid positional argument value (ex: stdin), so treat this
            // as a positional argument if there are any positionals left to fill.
            return ProcessPositionalArgument(currArg);
        }

        // No positional argument remaining. In stopOnUnknown mode this token isn't ours;
        // back up and let the next pass deal with it.
        if (m_stopOnUnknown)
        {
            return BackUpAndStop();
        }

        return ArgumentException(Localization::WSLCCLI_InvalidArgumentSpecifierError(currArg));
    }

    // Single '-' that is 2 characters or more means this must be an alias or collection of alias flags.
    if (currArg[1] != WSLC_CLI_ARG_ID_CHAR)
    {
        return ProcessAliasArgument(currArg);
    }

    // The currentArg must be a named argument.
    return ProcessNamedArgument(currArg);
}

// Assumes non-empty.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessPositionalArgument(const std::wstring_view& currArg)
{
    WI_ASSERT(!currArg.empty());

    const Argument* nextPositional = NextPositional();
    if (!nextPositional)
    {
        return ArgumentException(Localization::WSLCCLI_ExtraPositionalError(currArg));
    }

    // First positional found is the anchor positional.
    if (!m_anchorPositional.has_value())
    {
        m_anchorPositional = Argument(*nextPositional);
    }

    m_executionArgs.Add(nextPositional->Type(), std::wstring{currArg});
    return {};
}

// Assumes one positional has already been found and therefore there are no remaining Kind Value/Flag arguments.
// Only Kind::Positional or Kind::Forward arguments should remain.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessAnchoredPositionals(const std::wstring_view& currArg)
{
    WI_ASSERT(m_anchorPositional.has_value());

    // If we haven't reached the limit for the anchor positional, treat this as another anchor positional.
    // Unlimited anchors are never full and therefore always treat subsequent positionals as anchors.
    if (m_anchorPositional.value().IsUnlimited() || (m_executionArgs.Count(m_anchorPositional.value().Type()) < 1))
    {
        m_executionArgs.Add(m_anchorPositional.value().Type(), std::wstring{currArg});
        return {};
    }

    // There are three possibilities for this argument:
    // 1) It is another positional argument (ex: run <imagename> <command>)
    // 2) It is a forwarded argument set that could be anything (most likely)
    // 3) It is an input error and there should be no such argument.

    // Check next positional.
    const Argument* nextPositional = NextPositional();
    if (nextPositional)
    {
        m_executionArgs.Add(nextPositional->Type(), std::wstring{currArg});
        return {};
    }

    // Handle case where we expect a positional but don't find one - check forwarded args.

    // Check for forwarded arg existence.
    if (m_forwardArgs.empty())
    {
        return ArgumentException(Localization::WSLCCLI_CommandHasNoForwardArgumentsError(currArg));
    }

    // currArg is the first forwarded argument
    // All the rest of the args are forward args.
    std::vector<std::wstring> forwardedArgs;
    forwardedArgs.emplace_back(std::wstring{currArg});
    while (m_invocationItr != m_invocation.end())
    {
        forwardedArgs.emplace_back(std::wstring{*m_invocationItr});
        ++m_invocationItr;
    }

    m_executionArgs.Add(m_forwardArgs.front().Type(), std::move(forwardedArgs));
    return {};
}

// Assumes argument begins with '-' and is at least 2 characters.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessAliasArgument(const std::wstring_view& currArg)
{
    WI_ASSERT(currArg.length() >= 2 && currArg[0] == WSLC_CLI_ARG_ID_CHAR && currArg[1] != WSLC_CLI_ARG_ID_CHAR);

    // This may be a collection of boolean alias flags.
    // Helper to find an argument by alias starting at a specific position.
    auto findArgumentByAlias = [this](const std::wstring_view& str, size_t startPos, size_t& aliasLength) -> const Argument* {
        for (const auto& arg : m_standardArgs)
        {
            const auto& alias = arg.Alias();
            if (alias.empty())
            {
                continue;
            }

            if (startPos + alias.length() <= str.length() && str.compare(startPos, alias.length(), alias) == 0)
            {
                aliasLength = alias.length();
                return &arg;
            }
        }

        return nullptr;
    };

    // Find the first alias starting at position 1 (after the '-')
    size_t aliasLength = 0;
    const Argument* firstArg = findArgumentByAlias(currArg, 1, aliasLength);
    if (!firstArg)
    {
        // Leading alias is unknown. In stopOnUnknown mode nothing has been added
        // to m_executionArgs for this token yet, so it is safe to back up and stop.
        if (m_stopOnUnknown)
        {
            return BackUpAndStop();
        }

        return ArgumentException(Localization::WSLCCLI_InvalidAliasError(currArg));
    }

    if (IsUnsupportedArgument(firstArg->Type()))
    {
        ThrowUnsupportedArgument(*firstArg);
    }

    // Position after the first alias
    size_t currentPos = 1 + aliasLength;

    // Check if this argument expects a value
    if (firstArg->Kind() == Kind::Value)
    {
        const auto type = ResolveArgumentType(*firstArg);

        // Kind::Value is only allowed if it's the last flag (no more characters after it, or '=' follows)
        if (currentPos >= currArg.length())
        {
            // No more characters - value should be in next argument
            return {type, currArg};
        }

        if (currArg[currentPos] != WSLC_CLI_ARG_SPLIT_CHAR)
        {
            // There are more characters but it's not '=' - this is invalid
            return ArgumentException(Localization::WSLCCLI_ValueMustBeLastInAliasChainError(currArg), *firstArg);
        }

        // Value is adjoined after '='
        ProcessAdjoinedValue(type, currArg.substr(currentPos + 1));
        return {};
    }

    const auto firstType = ResolveArgumentType(*firstArg);

    // Boolean flag - check for adjoined boolean value (e.g., -a=true or -a=false).
    if (currentPos < currArg.length() && currArg[currentPos] == WSLC_CLI_ARG_SPLIT_CHAR)
    {
        return ApplyFlagValue(firstType, currArg.substr(currentPos + 1), currArg);
    }

    // No adjoined value — add the flag as true.
    SetFlag(firstType, true);

    // Process remaining adjoined flags
    while (currentPos < currArg.length())
    {
        const Argument* nextArg = findArgumentByAlias(currArg, currentPos, aliasLength);

        if (!nextArg)
        {
            return ArgumentException(Localization::WSLCCLI_AdjoinedNotFoundError(currArg));
        }

        if (IsUnsupportedArgument(nextArg->Type()))
        {
            ThrowUnsupportedArgument(*nextArg);
        }

        // Update position before checking Kind
        size_t nextPos = currentPos + aliasLength;

        if (nextArg->Kind() == Kind::Value)
        {
            const auto type = ResolveArgumentType(*nextArg);

            // Kind::Value is only allowed if it's the last flag
            if (nextPos >= currArg.length())
            {
                // No more characters - value should be in next argument
                return {type, currArg};
            }

            if (currArg[nextPos] != WSLC_CLI_ARG_SPLIT_CHAR)
            {
                // There are more characters but it's not '=' - this is invalid
                return ArgumentException(Localization::WSLCCLI_ValueMustBeLastInAliasChainError(currArg), *nextArg);
            }

            // Value is adjoined after '='
            ProcessAdjoinedValue(type, currArg.substr(nextPos + 1));
            return {};
        }

        const auto type = ResolveArgumentType(*nextArg);

        // Boolean flag in chain — check for adjoined boolean value.
        if (nextPos < currArg.length() && currArg[nextPos] == WSLC_CLI_ARG_SPLIT_CHAR)
        {
            return ApplyFlagValue(type, currArg.substr(nextPos + 1), currArg);
        }

        SetFlag(type, true);
        currentPos = nextPos;
    }

    return {};
}

// Assumes the arg value begins with -- and is at least 2 characters long.
ParseArgumentsStateMachine::State ParseArgumentsStateMachine::ProcessNamedArgument(const std::wstring_view& currArg)
{
    WI_ASSERT(currArg.starts_with(L"--"));

    if (currArg.length() == 2)
    {
        // Bare '--': not a name we recognize. In stopOnUnknown mode hand it off
        // to the next pass; otherwise it's a malformed token at this level.
        if (m_stopOnUnknown)
        {
            return BackUpAndStop();
        }

        return ArgumentException(Localization::WSLCCLI_MissingArgumentNameError(currArg));
    }

    // This is an arg name, find it and process its value if needed.
    // Skip the double arg identifier chars.
    size_t argStart = currArg.find_first_not_of(WSLC_CLI_ARG_ID_CHAR);
    std::wstring_view argName = currArg.substr(argStart);
    bool argFound = false;

    bool hasAdjoinedValue = false;
    std::wstring_view argValue;
    size_t splitChar = argName.find_first_of(WSLC_CLI_ARG_SPLIT_CHAR);
    if (splitChar != std::string::npos)
    {
        // There is an '=' in this arg, it has an adjoined value, split it out.
        hasAdjoinedValue = true;
        argValue = argName.substr(splitChar + 1);
        argName = argName.substr(0, splitChar);
    }

    // Find a matching standard arg with this name.
    for (const auto& arg : m_standardArgs)
    {
        if (string::IsEqual(argName, arg.Name()))
        {
            if (IsUnsupportedArgument(arg.Type()))
            {
                ThrowUnsupportedArgument(arg);
            }

            const auto type = ResolveArgumentType(arg);

            // Found a match, process by kind.
            if (arg.Kind() == Kind::Flag)
            {
                if (hasAdjoinedValue)
                {
                    return ApplyFlagValue(type, argValue, currArg);
                }

                SetFlag(type, true);
                return {};
            }

            // Not a Flag, must be a Value, and therefore must have a value provided.
            if (hasAdjoinedValue)
            {
                ProcessAdjoinedValue(type, argValue);
                return {};
            }

            // The value should be the next argument.
            return {type, currArg};
        }
    }

    // Unknown name. In stopOnUnknown mode hand it off to the next pass.
    if (m_stopOnUnknown)
    {
        return BackUpAndStop();
    }

    return ArgumentException(Localization::WSLCCLI_InvalidNameError(currArg));
}

void ParseArgumentsStateMachine::ProcessAdjoinedValue(ArgType type, std::wstring_view value)
{
    // If the adjoined value is wrapped in quotes, strip them off.
    AddValue(type, std::wstring{StripSurroundingQuotes(value)});
}
} // namespace wsl::windows::wslc
