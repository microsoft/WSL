/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ShellCommand.h

Abstract:

    This file contains the ShellCommand definition

--*/
#pragma once

#include "precomp.h"
#include "ICommand.h"

namespace wsl::windows::wslc::commands {

// wslc shell list
class ShellListCommand : public ICommand
{
public:
    std::string Name() const override
    {
        return "list";
    }
    std::string Description() const override
    {
        return "Lists all the shell sessions.";
    }
    std::vector<std::string> Options() const override
    {
        return {
            "-v, --verbose: Provides additional details in the output.",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddArgument(m_verbose, L"--verbose", 'v');
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    bool m_verbose{};
};

// wslc shell attach
class ShellAttachCommand : public ICommand
{
public:
    std::string Name() const override
    {
        return "attach";
    }
    std::string Description() const override
    {
        return "Attaches to a running shell session.";
    }
    std::vector<std::string> Options() const override
    {
        return {
            "name (pos. 0): Name of the shell session to attach to.",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_name}, 0);
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    std::string m_name;
};

// wslc shell
class ShellCommand : public ICommand
{
public:
    std::string Name() const override
    {
        return "shell";
    }
    std::string Description() const override
    {
        return "Manage shell sessions.";
    }
    std::vector<std::string> Options() const override
    {
        return {
            m_list.GetShortDescription(),
            m_attach.GetShortDescription(),
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_subverb}, 0);
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    std::string m_subverb;
    ShellListCommand m_list;
    ShellAttachCommand m_attach;
};
} // namespace wsl::windows::wslc::commands
