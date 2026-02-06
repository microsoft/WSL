// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "precomp.h"
#include "Argument.h"
#include "Exceptions.h"
#include "ArgumentTypes.h"
#include "CLIExecutionContext.h"
#include "Invocation.h"
#include "ArgumentParser.h"

#include <initializer_list>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc
{
    // Flags to control the behavior of the command output.
    enum class CommandOutputFlags : int
    {
        None = 0x0,
    };

    DEFINE_ENUM_FLAG_OPERATORS(CommandOutputFlags);

    struct Command
    {
        // Controls the visibility of the field.
        enum class Visibility
        {
            // Shown in help.
            Show,
            // Not shown in help.
            Hidden,
        };

        Command(std::wstring_view name, std::wstring parent) :
            Command(name, {}, parent, Command::Visibility::Show) {}
        Command(std::wstring_view name, std::wstring parent, Command::Visibility visibility) :
            Command(name, {}, parent, visibility, CommandOutputFlags::None) {}
        Command(std::wstring_view name, std::wstring parent, CommandOutputFlags outputFlags) :
            Command(name, {}, parent, Command::Visibility::Show, outputFlags) {}
        Command(std::wstring_view name, std::vector<std::wstring_view> aliases, std::wstring parent) :
            Command(name, aliases, parent, Command::Visibility::Show) {}
        Command(std::wstring_view name, std::vector<std::wstring_view> aliases, std::wstring parent, CommandOutputFlags outputFlags) :
            Command(name, aliases, parent, Command::Visibility::Show, outputFlags) {}
        Command(std::wstring_view name, std::vector<std::wstring_view> aliases, std::wstring parent, Command::Visibility visibility) :
            Command(name, aliases, parent, visibility, CommandOutputFlags::None) {}

        Command(std::wstring_view name,
            std::vector<std::wstring_view> aliases,
            std::wstring parent,
            Command::Visibility visibility,
            CommandOutputFlags outputFlags);

        virtual ~Command() = default;

        Command(const Command&) = default;
        Command& operator=(const Command&) = default;

        Command(Command&&) = default;
        Command& operator=(Command&&) = default;

        // The character used to split between commands and their parents in FullName.
        constexpr static wchar_t ParentSplitChar = L':';

        std::wstring_view Name() const { return m_name; }
        const std::vector<std::wstring_view>& Aliases() const& { return m_aliases; }
        const std::wstring& FullName() const { return m_fullName; }
        Command::Visibility GetVisibility() const;
        CommandOutputFlags GetOutputFlags() const { return m_outputFlags; }

        virtual std::vector<std::unique_ptr<Command>> GetCommands() const { return {}; }
        virtual std::vector<Argument> GetArguments() const { return {}; }
        std::vector<std::unique_ptr<Command>> GetVisibleCommands() const;
        std::vector<Argument> GetVisibleArguments() const;

        virtual std::wstring_view ShortDescription() const = 0;
        virtual std::wstring_view LongDescription() const = 0;

        virtual void OutputIntroHeader() const;
        virtual void OutputHelp(const CommandException* exception = nullptr) const;

        virtual std::unique_ptr<Command> FindSubCommand(Invocation& inv) const;
        virtual void ParseArguments(Invocation& inv, Args& execArgs) const;
        virtual void ValidateArguments(Args& execArgs) const;

        virtual void Execute(CLIExecutionContext& context) const;

    protected:
        void SelectCurrentCommandIfUnrecognizedSubcommandFound(bool value);

        virtual void ValidateArgumentsInternal(Args& execArgs) const;
        virtual void ExecuteInternal(CLIExecutionContext& context) const;

    private:
        std::wstring_view m_name;
        std::wstring m_fullName;
        std::vector<std::wstring_view> m_aliases;
        Command::Visibility m_visibility;
        CommandOutputFlags m_outputFlags;
        bool m_selectCurrentCommandIfUnrecognizedSubcommandFound = false;
        std::wstring m_commandArguments;
    };

    template <typename Container>
    Container InitializeFromMoveOnly(std::initializer_list<typename Container::value_type> il)
    {
        using String = typename Container::value_type;
        Container result;

        for (const auto& v : il)
        {
            result.emplace_back(std::move(*const_cast<String*>(&v)));
        }

        return result;
    }


    int Execute(CLIExecutionContext& context, std::unique_ptr<Command>& command);
}