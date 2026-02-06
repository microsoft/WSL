// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "FileLogger.h"
#include "StringUtilities.h"
#include "DateTime.h"

#include <wil/result_macros.h>

#include <shtypes.h>
#include <knownfolders.h>
#include <shlobj.h> 
#include <corecrt_io.h>

using namespace wsl::windows::wslc::util;

namespace wsl::windows::wslc::logging
{
    using namespace std::chrono_literals;

    namespace
    {
        static constexpr std::wstring_view s_fileLoggerDefaultFilePrefix = L"wslc";
        static constexpr std::wstring_view s_fileLoggerDefaultFileExt = L".log";
        static constexpr std::wstring_view s_fileLoggerPrefixName = L"Microsoft\\WSLC\\Logs";
        static constexpr uint32_t s_maxFileSizeInMB = 1024 * 1024 * 16;
        static constexpr uint32_t s_maxTotalLogSizeInMB = 1024 * 1024 * 32;
        static constexpr uint32_t s_maxLogFileCount = 10;
        static constexpr std::chrono::hours s_hoursToRetainLogs = 7 * 24h;

        // Send to a string first to create a single block to write to a file.
        std::wstring ToLogLine(Channel channel, std::wstring_view message)
        {
            std::wostringstream strstr;
            strstr << std::chrono::system_clock::now() << L" [" << std::setw(GetMaxChannelNameLength()) << std::left << std::setfill(L' ') << GetChannelName(channel) << L"] " << message;
            return std::move(strstr).str();
        }

        // Determines the difference between the given position and the maximum as an offset.
        std::wofstream::off_type CalculateDiff(const std::wofstream::pos_type& position, std::wofstream::off_type maximum)
        {
            auto offsetPosition = static_cast<std::wofstream::off_type>(position);
            return maximum > offsetPosition ? maximum - offsetPosition : 0;
        }

        // Limitations on a set of files.
        // Any value that is 0 is treated as no limit.
        struct FileLimits
        {
            std::chrono::hours Age = 0h;
            uint32_t TotalSizeInMB = 0;
            size_t Count = 0;
        };

        std::filesystem::path GetKnownFolderPath(const KNOWNFOLDERID& id)
        {
            wil::unique_cotaskmem_string knownFolder = nullptr;
            THROW_IF_FAILED(SHGetKnownFolderPath(id, KF_FLAG_NO_ALIAS | KF_FLAG_DONT_VERIFY | KF_FLAG_NO_PACKAGE_REDIRECTION, NULL, &knownFolder));
            return knownFolder.get();
        }

        std::filesystem::path GetDefaultLogDirectory()
        {
            auto basePath = GetKnownFolderPath(FOLDERID_LocalAppData);
            basePath /= s_fileLoggerPrefixName;
            
            // Create directory if it doesn't exist.
            if (std::filesystem::exists(basePath) && !std::filesystem::is_directory(basePath))
            {
                std::filesystem::remove(basePath);
            }

            std::filesystem::create_directories(basePath);
            return basePath;
        }

        // Information about a specific file.
        struct FileInfo
        {
            std::filesystem::path Path;
            std::filesystem::file_time_type LastWriteTime{};
            uintmax_t Size = 0;
        };

        std::vector<FileInfo> GetFileInfoFor(const std::filesystem::path& directory)
        {
            std::vector<FileInfo> result;

            for (const auto& file : std::filesystem::directory_iterator{ directory })
            {
                if (file.is_regular_file())
                {
                    result.emplace_back(FileInfo{ file.path(), file.last_write_time(), file.file_size() });
                }
            }

            return result;
        }

        void FilterToFilesExceedingLimits(std::vector<FileInfo>& files, const FileLimits& limits)
        {
            auto now = std::filesystem::file_time_type::clock::now();
            std::chrono::hours ageLimit = limits.Age;
            static_assert(sizeof(uintmax_t) >= 8);
            uintmax_t totalSizeLimit = static_cast<uintmax_t>(limits.TotalSizeInMB) << 20;
            size_t countLimit = limits.Count;

            // Sort with oldest first so that we can work backward to find the cutoff
            std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) { return a.LastWriteTime < b.LastWriteTime; });

            // Walk the list backward until we find the first entry that goes over one of the limits
            size_t i = files.size();
            uintmax_t totalSize = 0;
            for (; i > 0; --i)
            {
                const FileInfo& current = files[i - 1];

                if (totalSizeLimit != 0)
                {
                    totalSize += current.Size;
                    if (totalSize > totalSizeLimit)
                    {
                        break;
                    }
                }

                if (countLimit != 0 && (files.size() - i + 1) > countLimit)
                {
                    break;
                }

                if (ageLimit != 0h && now - current.LastWriteTime > ageLimit)
                {
                    break;
                }
            }

            files.resize(i);
        }
    }

    FileLogger::FileLogger() : FileLogger(s_fileLoggerDefaultFilePrefix) {}

    FileLogger::FileLogger(const std::wstring_view fileNamePrefix)
    {
        m_name = L"file";
        m_filePath = GetDefaultLogDirectory();
        m_filePath /= fileNamePrefix.data() + (L'-' + GetCurrentTimeForFilename() + s_fileLoggerDefaultFileExt.data());
        InitializeDefaultMaximumFileSize();
        OpenFileLoggerStream();
    }

    FileLogger::~FileLogger()
    {
        m_stream.flush();
        // When std::wofstream is constructed from an existing File handle, it does not call fclose on destruction
        // Only calling close() explicitly will close the file handle.
        m_stream.close();
    }

    FileLogger& FileLogger::SetMaximumSize(std::wofstream::off_type maximumSize)
    {
        THROW_HR_IF(E_INVALIDARG, maximumSize < 0);
        m_maximumSize = maximumSize;
        return *this;
    }

    std::wstring FileLogger::GetNameForPath(const std::filesystem::path& filePath)
    {
        return L"file :: " + filePath.wstring();
    }

    std::wstring_view FileLogger::DefaultPrefix()
    {
        return s_fileLoggerDefaultFilePrefix;
    }

    std::wstring_view FileLogger::DefaultExt()
    {
        return s_fileLoggerDefaultFileExt;
    }

    std::wstring FileLogger::GetName() const
    {
        return m_name;
    }

    void FileLogger::Write(Channel channel, Level level, std::wstring_view message) noexcept try
    {
        std::wstring log = ToLogLine(channel, message);
        WriteDirect(channel, level, log);
    }
    catch (...) {}

    void FileLogger::WriteDirect(Channel, Level, std::wstring_view message) noexcept try
    {
        HandleMaximumFileSize(message);
        m_stream << message << std::endl;
    }
    catch (...) {}

    void FileLogger::SetTag(Tag tag) noexcept try
    {
        if (tag == Tag::HeadersComplete)
        {
            auto currentPosition = m_stream.tellp();
            if (currentPosition != std::wofstream::pos_type{ -1 })
            {
                m_headersEnd = currentPosition;
            }
        }
    }
    catch (...) {}

    void FileLogger::Add()
    {
        Log().AddLogger(std::make_unique<FileLogger>());
    }

    void FileLogger::BeginCleanup()
    {
        BeginCleanup(GetDefaultLogDirectory());
    }

    void FileLogger::BeginCleanup(const std::filesystem::path& filePath)
    {
        std::thread([filePath]()
            {
                try
                {
                    FileLimits fileLimits;
                    fileLimits.Age = s_hoursToRetainLogs;
                    fileLimits.TotalSizeInMB = s_maxTotalLogSizeInMB;
                    fileLimits.Count = s_maxLogFileCount;

                    auto filesInPath = GetFileInfoFor(filePath);
                    FilterToFilesExceedingLimits(filesInPath, fileLimits);

                    for (const auto& file : filesInPath)
                    {
                        std::filesystem::remove(file.Path);
                    }
                }
                // Just throw out everything
                catch (...) {}
            }).detach();
    }

    void FileLogger::OpenFileLoggerStream() 
    {
        // Prevent other writers to our log file, but allow readers
        FILE* filePtr = _wfsopen(m_filePath.wstring().c_str(), L"w", _SH_DENYWR);

        if (filePtr)
        {
            auto closeFile = wil::scope_exit([&]() { fclose(filePtr); });

            // Prevent inheritance to ensure log file handle is not opened by other processes
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(filePtr))), HANDLE_FLAG_INHERIT, 0));

            m_stream = std::wofstream{ filePtr };
            closeFile.release();
        }
        else
        {
            WSLC_LOG(Core, Error, << "Failed to open log file " << m_filePath.wstring());
            throw std::system_error(errno, std::generic_category());
        }
    }

    void FileLogger::InitializeDefaultMaximumFileSize()
    {
        m_maximumSize = s_maxFileSizeInMB;
    }

    void FileLogger::HandleMaximumFileSize(std::wstring_view& currentLog)
    {
        if (m_maximumSize == 0)
        {
            return;
        }

        auto maximumLogSize = static_cast<size_t>(CalculateDiff(m_headersEnd, m_maximumSize));

        // In the event that a single log is larger than the maximum
        if (currentLog.size() > maximumLogSize)
        {
            currentLog = currentLog.substr(0, maximumLogSize);
            WrapLogFile();
            return;
        }

        auto currentPosition = m_stream.tellp();
        if (currentPosition == std::wofstream::pos_type{ -1 })
        {
            // The expectation is that if the stream is in an error state the write won't actually happen.
            return;
        }

        auto availableSpace = static_cast<size_t>(CalculateDiff(currentPosition, m_maximumSize));

        if (currentLog.size() > availableSpace)
        {
            WrapLogFile();
            return;
        }
    }

    void FileLogger::WrapLogFile()
    {
        m_stream.seekp(m_headersEnd);
        // Yes, we may go over the size limit slightly due to this and the unaccounted for newlines
        m_stream << ToLogLine(Channel::Core, L"--- log file has wrapped ---") << std::endl;
    }
}
