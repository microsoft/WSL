// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "Logging.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace wsl::windows::wslc::logging
{
    // Logs to a file.
    struct FileLogger : public ILogger
    {
        FileLogger();
        explicit FileLogger(const std::wstring_view fileNamePrefix);

        ~FileLogger();

        FileLogger(const FileLogger&) = delete;
        FileLogger& operator=(const FileLogger&) = delete;

        FileLogger(FileLogger&&) = default;
        FileLogger& operator=(FileLogger&&) = default;

        // The default value for the maximum size comes from settings.
        // Setting the maximum size to 0 will disable the maximum.
        FileLogger& SetMaximumSize(std::ofstream::off_type maximumSize);

        static std::wstring GetNameForPath(const std::filesystem::path& filePath);

        static std::wstring_view DefaultPrefix();
        static std::wstring_view DefaultExt();

        // ILogger
        std::wstring GetName() const override;

        void Write(Channel channel, Level level, std::wstring_view message) noexcept override;

        void WriteDirect(Channel channel, Level level, std::wstring_view message) noexcept override;
        void SetTag(Tag tag) noexcept override;

        // Adds a FileLogger to the current Log
        static void Add();

        // Starts a background task to clean up old log files.
        static void BeginCleanup();
        static void BeginCleanup(const std::filesystem::path& filePath);

    private:
        std::wstring m_name;
        std::filesystem::path m_filePath;
        std::wofstream m_stream;
        std::wofstream::pos_type m_headersEnd = 0;
        std::wofstream::off_type m_maximumSize = 0;

        void OpenFileLoggerStream();

        // Initializes the default maximum file size.
        void InitializeDefaultMaximumFileSize();

        // Determines if the logger needs to wrap back to the beginning, doing so when needed.
        // May also shrink the given view if it exceeds the overall maximum.
        void HandleMaximumFileSize(std::wstring_view& currentLog);

        // Resets the log file state so that it will overwrite the data portion.
        void WrapLogFile();
    };
}
