// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sstream>
#include <optional>
#include <assert.h>

namespace wil {

#define STRING_TO_WSTRING_IMPL(Str) L##Str
#define STRING_TO_WSTRING(Str) STRING_TO_WSTRING_IMPL(Str)
#define TEXT(X) X
#define FAIL_FAST() raise(SIGABRT);
#define FAIL_FAST_CAUGHT_EXCEPTION() FAIL_FAST()
#define FAIL_FAST_IF(condition) \
    if ((condition)) \
    { \
        FAIL_FAST() \
    }

typedef void LogFunction(const char* message, const char* exceptionDescription) noexcept;
__declspec(selectany) LogFunction* g_LogExceptionCallback{};

namespace details {
    struct FailureInfo
    {
        const char* File;
        int Line;
        const char* Function;
    };
} // namespace details

class ResultException : public std::exception
{
public:
    ResultException(int result, details::FailureInfo info) noexcept : m_Result{result}, m_Info{info}
    {
    }

    ~ResultException() noexcept
    {
        delete[] m_What;
    }

    const char* what() const noexcept override
    {
        constexpr size_t bufferSize = 4096;
        if (m_What == nullptr)
        {
            m_What = new (std::nothrow) char[bufferSize]{};
            if (m_What == nullptr)
            {
                return strerror(m_Result);
            }

            snprintf(m_What, bufferSize, "%s @%s:%d (%s)\n", strerror(m_Result), m_Info.File, m_Info.Line, m_Info.Function);
        }

        return m_What;
    }

    int GetErrorCode() const noexcept
    {
        return m_Result;
    }

private:
    mutable char* m_What{};
    int m_Result;
    details::FailureInfo m_Info;
};

class ExceptionWithUserMessage : public std::exception
{
public:
    ExceptionWithUserMessage(std::string&& message) : m_message(std::move(message))
    {
    }

    const char* what() const noexcept override
    {
        return m_message.c_str();
    }

private:
    std::string m_message;
};

namespace details {
    template <typename TLambda>
    class lambda_call
    {
    public:
        lambda_call(const lambda_call&) = delete;
        lambda_call& operator=(const lambda_call&) = delete;
        lambda_call& operator=(lambda_call&& other) = delete;

        explicit lambda_call(TLambda&& lambda) noexcept : m_lambda(std::move(lambda))
        {
            static_assert(std::is_same<decltype(lambda()), void>::value, "scope_exit lambdas must not have a return value");
            static_assert(
                !std::is_lvalue_reference<TLambda>::value && !std::is_rvalue_reference<TLambda>::value,
                "scope_exit should only be directly used with a lambda");
        }

        lambda_call(lambda_call&& other) noexcept : m_lambda(std::move(other.m_lambda)), m_call(other.m_call)
        {
            other.m_call = false;
        }

        ~lambda_call() noexcept
        {
            reset();
        }

        // Ensures the scope_exit lambda will not be called
        void release() noexcept
        {
            m_call = false;
        }

        // Executes the scope_exit lambda immediately if not yet run; ensures it will not run again
        void reset() noexcept
        {
            if (m_call)
            {
                m_call = false;
                m_lambda();
            }
        }

        // Returns true if the scope_exit lambda is still going to be executed
        explicit operator bool() const noexcept
        {
            return m_call;
        }

    protected:
        TLambda m_lambda;
        bool m_call = true;
    };

    inline void ThrowErrorIf(bool condition, int error, FailureInfo info)
    {
        if (condition)
        {
            throw ::wil::ResultException(error, info);
        }
    }

    inline void LogFailure(const char* message, const char* exceptionDescription) noexcept
    {
        auto callback = g_LogExceptionCallback;
        if (callback != nullptr)
        {
            callback(message, exceptionDescription);
        }
        else
        {
            if (message != nullptr)
            {
                fputs(message, stderr);
                fputs("\n", stderr);
            }

            if (exceptionDescription != nullptr)
            {
                fputs("Exception: ", stderr);
                fputs(exceptionDescription, stderr);
                fputs("\n", stderr);
            }
        }
    }

    inline void LogCaughtException(const char* message)
    {
        try
        {
            throw;
        }
        catch (const std::exception& ex)
        {
            LogFailure(message, ex.what());
        }
        catch (...)
        {
            LogFailure(message, nullptr);
        }
    }
} // namespace details

inline int ResultFromCaughtException()
{
    try
    {
        throw;
    }
    catch (wil::ResultException& ex)
    {
        return ex.GetErrorCode();
    }
    catch (std::bad_alloc&)
    {
        return ENOMEM;
    }
    catch (...)
    {
    }

    // Unknown exception type.
    return EINVAL;
}

#define __WIL_ERROR_INFO {__FILE__, __LINE__, __FUNCTION__}

#define THROW_ERRNO(Error) throw ::wil::ResultException(Error, __WIL_ERROR_INFO)
#define THROW_USER_ERROR(Message) throw ::wil::ExceptionWithUserMessage((Message))
#define THROW_ERRNO_IF(Error, Condition) ::wil::details::ThrowErrorIf((Condition), (Error), __WIL_ERROR_INFO)
#define THROW_LAST_ERROR_IF(Condition) THROW_ERRNO_IF(errno, (Condition));
#define THROW_LAST_ERROR() THROW_ERRNO(errno);

#define THROW_INVALID() THROW_ERRNO(EINVAL)
#define THROW_UNEXCEPTED() THROW_ERRNO(EINVAL)
#define THROW_INVALID_IF(Condition) THROW_ERRNO_IF(EINVAL, (Condition))
#define THROW_UNEXPECTED_IF(Condition) THROW_ERRNO_IF(EINVAL, (Condition))

#define LOG_CAUGHT_EXCEPTION() ::wil::details::LogCaughtException(nullptr);
#define LOG_CAUGHT_EXCEPTION_MSG(msg) ::wil::details::LogCaughtException(msg);
#define RETURN_CAUGHT_EXCEPTION() return -::wil::ResultFromCaughtException()
#define CATCH_RETURN() \
    catch (...) \
    { \
        RETURN_CAUGHT_EXCEPTION(); \
    }
#define CATCH_RETURN_ERRNO() \
    catch (...) \
    { \
        LOG_CAUGHT_EXCEPTION(); \
        errno = ::wil::ResultFromCaughtException(); \
        return -1; \
    }

#define CATCH_LOG() \
    catch (...) \
    { \
        LOG_CAUGHT_EXCEPTION(); \
    }
#define CATCH_LOG_MSG(msg) \
    catch (...) \
    { \
        LOG_CAUGHT_EXCEPTION_MSG(msg); \
    }

class unique_dir
{
public:
    static constexpr DIR* invalid_dir = nullptr;

    unique_dir(DIR* dir = invalid_dir) noexcept : m_Dir{dir}
    {
    }

    ~unique_dir() noexcept
    {
        reset();
    }

    unique_dir(const unique_dir&) = delete;
    unique_dir& operator=(const unique_dir&) = delete;

    unique_dir(unique_dir&& other) noexcept : m_Dir{other.m_Dir}
    {
        other.m_Dir = invalid_dir;
    }

    unique_dir& operator=(unique_dir&& other) noexcept
    {
        std::swap(m_Dir, other.m_Dir);
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return m_Dir != invalid_dir;
    }

    DIR* get() const noexcept
    {
        return m_Dir;
    }

    void reset(DIR* dir = invalid_dir) noexcept
    {
        if (m_Dir != invalid_dir)
        {
            closedir(m_Dir);
        }

        m_Dir = dir;
    }

    DIR* release() noexcept
    {
        DIR* dir = m_Dir;
        m_Dir = invalid_dir;
        return dir;
    }

    friend void swap(unique_dir& dir1, unique_dir& dir2)
    {
        std::swap(dir1.m_Dir, dir2.m_Dir);
    }

private:
    DIR* m_Dir;
};

class unique_fd
{
public:
    static constexpr int invalid_fd = -1;

    unique_fd(int fd = invalid_fd) noexcept : m_Fd{fd}
    {
    }

    ~unique_fd() noexcept
    {
        reset();
    }

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    unique_fd(unique_fd&& other) noexcept : m_Fd{other.m_Fd}
    {
        other.m_Fd = invalid_fd;
    }

    unique_fd& operator=(unique_fd&& other) noexcept
    {
        std::swap(m_Fd, other.m_Fd);
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return m_Fd >= 0;
    }

    int get() const noexcept
    {
        return m_Fd;
    }

    void reset(int fd = invalid_fd) noexcept
    {
        if (m_Fd >= 0)
        {
            close(m_Fd);
        }

        m_Fd = fd;
    }

    int release() noexcept
    {
        int fd = m_Fd;
        m_Fd = invalid_fd;
        return fd;
    }

    friend void swap(unique_fd& fd1, unique_fd& fd2)
    {
        std::swap(fd1.m_Fd, fd2.m_Fd);
    }

private:
    int m_Fd;
};

class unique_pipe
{
public:
    unique_pipe() = default;

    unique_pipe(unique_fd&& readFd, unique_fd&& writeFd) noexcept : m_Read(std::move(readFd)), m_Write(std::move(writeFd))
    {
    }

    unique_pipe(const unique_pipe&) = delete;
    unique_pipe& operator=(const unique_pipe&) = delete;

    unique_pipe(unique_pipe&& other) noexcept
    {
        m_Read = std::move(other.m_Read);
        m_Write = std::move(other.m_Write);
    }

    unique_pipe& operator=(unique_pipe&& other) noexcept
    {
        m_Read = std::move(other.m_Read);
        m_Write = std::move(other.m_Write);
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return m_Read || m_Write;
    }

    unique_fd& read()
    {
        return m_Read;
    }

    unique_fd& write()
    {
        return m_Write;
    }

    std::pair<unique_fd, unique_fd> release() noexcept
    {
        auto fds = std::make_pair(std::move(m_Read), std::move(m_Write));
        return fds;
    }

    friend void swap(unique_pipe& left, unique_pipe& right)
    {
        std::swap(left.m_Read, right.m_Read);
        std::swap(left.m_Write, right.m_Write);
    }

    static unique_pipe create(int flags)
    {
        int pipe[2] = {-1, -1};
        if (pipe2(pipe, flags) < -1)
        {
            THROW_ERRNO(errno);
        }

        return unique_pipe(unique_fd(pipe[0]), unique_fd(pipe[1]));
    }

private:
    unique_fd m_Read;
    unique_fd m_Write;
};

class unique_file
{
public:
    static constexpr FILE* invalid_file = nullptr;

    unique_file(FILE* file = invalid_file) noexcept : m_File{file}
    {
    }

    ~unique_file() noexcept
    {
        reset();
    }

    unique_file(const unique_file&) = delete;
    unique_file& operator=(const unique_file&) = delete;

    unique_file(unique_file&& other) noexcept : m_File{other.m_File}
    {
        other.m_File = invalid_file;
    }

    unique_file& operator=(unique_file&& other) noexcept
    {
        std::swap(m_File, other.m_File);
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return m_File != invalid_file;
    }

    FILE* get() const noexcept
    {
        return m_File;
    }

    void reset(FILE* file = invalid_file) noexcept
    {
        if (m_File != invalid_file)
        {
            fclose(m_File);
        }

        m_File = file;
    }

    FILE* release() noexcept
    {
        FILE* file = m_File;
        m_File = invalid_file;
        return file;
    }

    friend void swap(unique_file& file1, unique_file& file2)
    {
        std::swap(file1.m_File, file2.m_File);
    }

private:
    FILE* m_File;
};

/** Returns an object that executes the given lambda when destroyed.
Capture the object with 'auto'; use reset() to execute the lambda early or release() to avoid
execution.  Exceptions thrown in the lambda will fail-fast; use scope_exit_log to avoid. */
template <typename TLambda>
[[nodiscard]] inline auto scope_exit(TLambda&& lambda) noexcept
{
    return details::lambda_call<TLambda>(std::forward<TLambda>(lambda));
}

namespace details {
    template <unsigned long long flag>
    struct verify_single_flag_helper
    {
        static_assert((flag != 0) && ((flag & (flag - 1)) == 0), "Single flag expected, zero or multiple flags found");
        static const unsigned long long value = flag;
    };

// Use size-specific casts to avoid sign extending numbers -- avoid warning C4310: cast truncates constant value
#define __WI_MAKE_UNSIGNED(val) \
    (sizeof(val) == 1   ? static_cast<unsigned char>(val) \
     : sizeof(val) == 2 ? static_cast<unsigned short>(val) \
     : sizeof(val) == 4 ? static_cast<unsigned long>(val) \
                        : static_cast<unsigned long long>(val))
#define __WI_IS_UNSIGNED_SINGLE_FLAG_SET(val) ((val) && !((val) & ((val) - 1)))
#define __WI_IS_SINGLE_FLAG_SET(val) __WI_IS_UNSIGNED_SINGLE_FLAG_SET(__WI_MAKE_UNSIGNED(val))

    template <typename TVal, typename TFlags>
    inline constexpr bool AreAllFlagsSetHelper(TVal val, TFlags flags)
    {
        return ((val & flags) == static_cast<decltype(val & flags)>(flags));
    }

    template <typename TVal>
    inline constexpr bool IsSingleFlagSetHelper(TVal val)
    {
        return __WI_IS_SINGLE_FLAG_SET(val);
    }

    template <typename TVal>
    inline constexpr bool IsClearOrSingleFlagSetHelper(TVal val)
    {
        return ((val == static_cast<std::remove_reference_t<TVal>>(0)) || IsSingleFlagSetHelper(val));
    }

    template <typename TVal, typename TMask, typename TFlags>
    inline constexpr void UpdateFlagsInMaskHelper(TVal& val, TMask mask, TFlags flags)
    {
        val = static_cast<std::remove_reference_t<TVal>>((val & ~mask) | (flags & mask));
    }

    template <long>
    struct variable_size;

    template <>
    struct variable_size<1>
    {
        typedef unsigned char type;
    };

    template <>
    struct variable_size<2>
    {
        typedef unsigned short type;
    };

    template <>
    struct variable_size<4>
    {
        typedef unsigned long type;
    };

    template <>
    struct variable_size<8>
    {
        typedef unsigned long long type;
    };

    template <typename T>
    struct variable_size_mapping
    {
        typedef typename variable_size<sizeof(T)>::type type;
    };
} // namespace details

/** Defines the unsigned type of the same width (1, 2, 4, or 8 bytes) as the given type.
This allows code to generically convert any enum class to it's corresponding underlying type. */
template <typename T>
using integral_from_enum = typename details::variable_size_mapping<T>::type;

#define WI_StaticAssertSingleBitSet(flag) \
    static_cast<decltype(flag)>(::wil::details::verify_single_flag_helper<static_cast<unsigned long long>(WI_EnumValue(flag))>::value)
#define WI_IsAnyFlagSet(val, flags) \
    (static_cast<decltype((val) & (flags))>(WI_EnumValue(val) & WI_EnumValue(flags)) != static_cast<decltype((val) & (flags))>(0))
#define WI_IsFlagSet(val, flag) WI_IsAnyFlagSet(val, WI_StaticAssertSingleBitSet(flag))
#define WI_AreAllFlagsClear(val, flags) \
    (static_cast<decltype((val) & (flags))>(WI_EnumValue(val) & WI_EnumValue(flags)) == static_cast<decltype((val) & (flags))>(0))
#define WI_IsAnyFlagClear(val, flags) (!wil::details::AreAllFlagsSetHelper(val, flags))
#define WI_IsFlagClear(val, flag) WI_AreAllFlagsClear(val, WI_StaticAssertSingleBitSet(flag))
#define WI_EnumValue(val) static_cast<::wil::integral_from_enum<decltype(val)>>(val)
//! Evaluates as true if every bitflag specified in `flags` is set within `val`.
#define WI_AreAllFlagsSet(val, flags) wil::details::AreAllFlagsSetHelper(val, flags)
//! Set zero or more bitflags specified by `flags` in the variable `var`.
#define WI_SetAllFlags(var, flags) ((var) |= (flags))
//! Set a single compile-time constant `flag` in the variable `var`.
#define WI_SetFlag(var, flag) WI_SetAllFlags(var, WI_StaticAssertSingleBitSet(flag))
//! Conditionally sets a single compile-time constant `flag` in the variable `var` only if `condition` is true.
#define WI_SetFlagIf(var, flag, condition) \
    do \
    { \
        if (condition) \
        { \
            WI_SetFlag(var, flag); \
        } \
    } while ((void)0, 0)
//! Clear zero or more bitflags specified by `flags` from the variable `var`.
#define WI_ClearAllFlags(var, flags) ((var) &= ~(flags))
//! Clear a single compile-time constant `flag` from the variable `var`.
#define WI_ClearFlag(var, flag) WI_ClearAllFlags(var, WI_StaticAssertSingleBitSet(flag))

#define WI_ASSERT(condition) assert(condition)

#define EMIT_USER_WARNING(Warning) \
    if (::wil::ScopedWarningsCollector::CanCollectWarning()) \
    { \
        ::wil::ScopedWarningsCollector::CollectWarning(Warning); \
    }

class ScopedWarningsCollector
{
public:
    ScopedWarningsCollector()
    {
        assert(!g_collectedWarnings.has_value());

        g_collectedWarnings.emplace();
    }

    ~ScopedWarningsCollector()
    {
        assert(g_collectedWarnings.has_value());

        g_collectedWarnings = {};
    }

    ScopedWarningsCollector(const ScopedWarningsCollector&) = delete;
    ScopedWarningsCollector(ScopedWarningsCollector&&) = delete;
    ScopedWarningsCollector& operator=(const ScopedWarningsCollector&) = delete;
    ScopedWarningsCollector& operator=(ScopedWarningsCollector&&) = delete;

    static bool CanCollectWarning()
    {
        return g_collectedWarnings.has_value();
    }

    static void CollectWarning(std::string&& warning)
    {
        assert(g_collectedWarnings.has_value());

        (*g_collectedWarnings) << std::move(warning) << "\n";
    }

    static std::string ConsumeWarnings()
    {
        if (!g_collectedWarnings.has_value())
        {
            return {};
        }

        auto warnings = g_collectedWarnings->str();

        g_collectedWarnings = std::stringstream{};

        return warnings;
    }

private:
    static thread_local std::optional<std::stringstream> g_collectedWarnings;
};

} // namespace wil
