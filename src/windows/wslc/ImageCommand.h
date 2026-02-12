/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageCommand.h

Abstract:

    This file contains the ImageCommand definition

--*/
#pragma once

#include "ICommand.h"

namespace wslc::commands {
// wslc image list
class ImageListCommand : public ICommand
{
public:
    std::string Name() const override
    {
        return "list";
    }
    std::string Description() const override
    {
        return "Lists all the locally present images.";
    }
    std::vector<std::string> Options() const override
    {
        return {
            "--format: Output formatting (json or table. Default: table)",
            "-q, --quiet: Outputs the image names only",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddArgument(wsl::shared::Utf8String{m_format}, L"--format", 'f');
        parser.AddArgument(m_quiet, L"--quiet", 'q');
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    std::string m_format;
    bool m_quiet{};
};

// wslc image pull
class ImagePullCommand : public ICommand
{
public:
    std::string Name() const override
    {
        return "pull";
    }
    std::string Description() const override
    {
        return "Pulls an image from a registry.";
    }
    std::vector<std::string> Options() const override
    {
        return {
            "image (pos. 0): Image name",
        };
    }
    void LoadArguments(wsl::shared::ArgumentParser& parser) override
    {
        parser.AddPositionalArgument(wsl::shared::Utf8String{m_image}, 0);
    }

protected:
    int ExecuteInternal(std::wstring_view commandLine, int parserOffset = 0) override;

private:
    std::string m_image;
};

// wslc image
class ImageCommand : public ICommand
{
public:
    std::string Name() const override
    {
        return "image";
    }
    std::string Description() const override
    {
        return "Manage images.";
    }
    std::vector<std::string> Options() const override
    {
        return {
            m_list.GetShortDescription(),
            m_pull.GetShortDescription(),
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
    ImageListCommand m_list;
    ImagePullCommand m_pull;
};
} // namespace wslc::commands
