// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#define WSLC_LOG_DIRECT(_logger_,_channel_,_level_,_outstream_) \
    do { \
        auto _wslc_log_channel = wsl::windows::wslc::logging::Channel:: _channel_; \
        auto _wslc_log_level = wsl::windows::wslc::logging::Level:: _level_; \
        auto& _wslc_log_log = _logger_; \
        if (_wslc_log_log.IsEnabled(_wslc_log_channel, _wslc_log_level)) \
        { \
            wsl::windows::wslc::logging::LoggingStream _wslc_log_strstr; \
            _wslc_log_strstr _outstream_; \
            _wslc_log_log.Write(_wslc_log_channel, _wslc_log_level, _wslc_log_strstr.str()); \
        } \
    } while (0, 0)

#define WSLC_LOG(_channel_,_level_,_outstream_) WSLC_LOG_DIRECT(wsl::windows::wslc::logging::Log(),_channel_,_level_,_outstream_)

// Consider using this macro when the string might be larger than 4K.
// The normal macro has some buffering that occurs; it can cut off larger strings and is slower.
#define WSLC_LOG_LARGE_STRING(_channel_,_level_,_headerStream_,_largeString_) \
    do { \
        auto _wslc_log_channel = wsl::windows::wslc::logging::Channel:: _channel_; \
        auto _wslc_log_level = wsl::windows::wslc::logging::Level:: _level_; \
        auto& _wslc_log_log = wsl::windows::wslc::logging::Log(); \
        if (_wslc_log_log.IsEnabled(_wslc_log_channel, _wslc_log_level)) \
        { \
            wsl::windows::wslc::logging::LoggingStream _wslc_log_strstr; \
            _wslc_log_strstr _headerStream_; \
            _wslc_log_log.Write(_wslc_log_channel, _wslc_log_level, _wslc_log_strstr.str()); \
            _wslc_log_log.WriteDirect(_wslc_log_channel, _wslc_log_level, _largeString_); \
        } \
    } while (0, 0)

namespace wsl::windows::wslc::logging
{
    // The channel that the log is from.
    // Channels enable large groups of logs to be enabled or disabled together.
    enum class Channel : uint32_t
    {
        None = 0x0,
        Fail = 0x1,
        CLI = 0x2,
        Core = 0x4,
        Service = 0x8,
        Task = 0x10,
        Debug = 0x20,
        All = 0xFFFFFFFF,
        Defaults = All,
    };

    DEFINE_ENUM_FLAG_OPERATORS(Channel);

    // Gets the channel's name as a string.
    std::wstring_view GetChannelName(Channel channel);

    // Gets the channel from it's name.
    Channel GetChannelFromName(std::wstring_view channel);

    // Gets the maximum channel name length in characters.
    size_t GetMaxChannelNameLength();

    // The level of the log.
    enum class Level
    {
        Verbose,
        Info,
        Warning,
        Error,
        Crit,
    };

    // Indicates a location of significance in the logging stream.
    enum class Tag
    {
        // The initial set of logging has been completed.
        HeadersComplete,
    };

    // The interface that a log target must implement.
    struct ILogger
    {
        virtual ~ILogger() = default;

        // Gets the name of the logger for internal use.
        virtual std::wstring GetName() const = 0;

        // Informs the logger of the given log.
        virtual void Write(Channel channel, Level level, std::wstring_view message) noexcept = 0;

        // Informs the logger of the given log with the intention that no buffering occurs (in winget code).
        virtual void WriteDirect(Channel channel, Level level, std::wstring_view message) noexcept = 0;

        // Indicates that the given tag location has occurred.
        virtual void SetTag(Tag) noexcept {}
    };

    // This type contains the set of loggers that diagnostic logging will be sent to.
    // Each binary that leverages it must configure any loggers and filters to their
    // desired level, as nothing is enabled by default.
    struct DiagnosticLogger
    {
        DiagnosticLogger() = default;

        ~DiagnosticLogger() = default;

        DiagnosticLogger(const DiagnosticLogger&) = delete;
        DiagnosticLogger& operator=(const DiagnosticLogger&) = delete;

        DiagnosticLogger(DiagnosticLogger&&) = delete;
        DiagnosticLogger& operator=(DiagnosticLogger&&) = delete;

        // Gets the singleton instance of this type.
        static DiagnosticLogger& GetInstance();

        // NOTE: The logger management functionality is *SINGLE THREAD SAFE*.
        //       This includes with logging itself.
        //       As it is not expected that adding/removing loggers is an
        //       extremely frequent operation, no care has been made to protect
        //       it from modifying loggers while logging may be occurring.

        // Adds a logger to the active set.
        void AddLogger(std::unique_ptr<ILogger>&& logger);

        // Determines if a logger with the given name is present.
        bool ContainsLogger(const std::wstring& name);

        // Removes a logger from the active set, returning it.
        std::unique_ptr<ILogger> RemoveLogger(const std::wstring& name);

        // Removes all loggers.
        void RemoveAllLoggers();

        // Enables the given channel(s), in addition to the currently enabled channels.
        void EnableChannel(Channel channel);

        // The given channel mask will become the only enabled channels.
        void SetEnabledChannels(Channel channel);

        // Disables the given channel.
        void DisableChannel(Channel channel);

        // Sets the enabled level.
        // All levels above this level will be enabled.
        // For example; SetLevel(Verbose) will enable all logs.
        void SetLevel(Level level);

        // Gets the enabled level.
        Level GetLevel() const;

        // Checks whether a given channel and level are enabled.
        bool IsEnabled(Channel channel, Level level) const;

        // Writes a log line, if the given channel and level are enabled.
        void Write(Channel channel, Level level, std::wstring_view message);

        // Writes a log line, if the given channel and level are enabled.
        // Use to make large logs more efficient by writing directly to the output streams.
        void WriteDirect(Channel channel, Level level, std::wstring_view message);

        // Indicates that the given tag location has occurred.
        void SetTag(Tag tag);

    private:

        std::vector<std::unique_ptr<ILogger>> m_loggers;
        Channel m_enabledChannels = Channel::None;
        Level m_enabledLevel = Level::Info;
    };

    DiagnosticLogger& Log();

    // Calls the various stream format functions to produce an 8 character hexadecimal output.
    std::wostream& SetHRFormat(std::wostream& out);

    // This type allows us to override the default behavior of output operators for logging.
    struct LoggingStream
    {
        friend wsl::windows::wslc::logging::LoggingStream& operator<<(wsl::windows::wslc::logging::LoggingStream& out, const std::filesystem::path& path)
        {
            out.m_out << path.c_str();
            return out;
        }

        // Enums
        template <typename T>
        friend std::enable_if_t<std::is_enum_v<std::decay_t<T>>, wsl::windows::wslc::logging::LoggingStream&>
            operator<<(wsl::windows::wslc::logging::LoggingStream& out, T t)
        {
            out.m_out << static_cast<std::underlying_type_t<std::decay_t<T>>>(t);
            return out;
        }

        // Everything else.
        template <typename T>
        friend std::enable_if_t<!std::disjunction_v<std::is_same<std::decay_t<T>, std::filesystem::path>, std::is_enum<std::decay_t<T>>>, wsl::windows::wslc::logging::LoggingStream&>
            operator<<(wsl::windows::wslc::logging::LoggingStream& out, T&& t)
        {
            out.m_out << std::forward<T>(t);
            return out;
        }

        std::wstring str() const { return m_out.str(); }

    private:
        std::wstringstream m_out;
    };
}

namespace std
{
    std::wostream& operator<<(std::wostream& out, const std::chrono::system_clock::time_point& time);
}
