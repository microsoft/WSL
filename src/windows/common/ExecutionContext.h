// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

namespace wsl::windows::common {

#define THROW_HR_WITH_USER_ERROR(Result, Message) \
    if (wsl::windows::common::ExecutionContext::ShouldCollectErrorMessage()) \
    { \
        ::wsl::windows::common::SetErrorMessage(Message); \
    } \
    THROW_HR(Result)

#define EMIT_USER_WARNING(Warning) \
    if (::wsl::windows::common::ExecutionContext* context = ::wsl::windows::common::ExecutionContext::Current(); context != nullptr) \
    { \
        context->EmitUserWarning(Warning); \
    }

/* List of ExecutionContext that can be passed to ExecutionContext().
 * Note: ExecutionContext makes the assumption that the parent context always has
 * a lower value than its child context.
 * (for instance RegisterDistro must be smaller than CreateInstance
 * because RegisterDistro is always CreateInstance's parent).
 */

enum Context : ULONGLONG
{
    Empty = 0x0,
    Wsl = 0x1,
    Wslg = 0x2,
    Bash = 0x4,
    WslConfig = 0x8,
    InstallDistro = 0x10,
    EnumerateDistros = 0x20,
    Service = 0x40,
    RegisterDistro = 0x80,
    CreateInstance = 0x100,
    AttachDisk = 0x200,
    DetachDisk = 0x400,
    CreateVm = 0x800,
    ParseConfig = 0x1000,
    ConfigureNetworking = 0x2000,
    ConfigureGpu = 0x4000,
    LaunchProcess = 0x8000,
    ConfigureDistro = 0x10000,
    CreateLxProcess = 0x20000,
    UnregisterDistro = 0x40000,
    ExportDistro = 0x80000,
    GetDistroConfiguration = 0x100000,
    GetDistroId = 0x200000,
    SetDefaultDistro = 0x400000,
    SetVersion = 0x800000,
    TerminateDistro = 0x1000000,
    RegisterLxBus = 0x2000000,
    MountDisk = 0x4000000,
    Plugin = 0x8000000,
    MoveDistro = 0x10000000,
    GetDefaultDistro = 0x20000000,
    DebugShell = 0x40000000,
    HCS = 0x80000000,
    HNS = 0x100000000,
    CallMsi = 0x200000000,
    Install = 0x4000000000,
    ReadDistroConfig = 0x8000000000,
    UpdatePackage = 0x10000000000,
    QueryLatestGitHubRelease = 0x20000000000,
    VerifyChecksum = 0x40000000000,
};

DEFINE_ENUM_FLAG_OPERATORS(Context)

struct Error
{
    HRESULT Code = E_UNEXPECTED;
    ULONGLONG Context = 0;
    std::optional<std::wstring> Message;
};

/*
 * The ExecutionContext class is a tool to automatically contextualize the errors
 * so they are returned to the user (and optionally with a specialized error message).
 *
 * When an ExecutionContext is declared in a scope, it will override g_currentContext (thread-local)
 * and keep a pointer to its parent scope (caller), if any.
 *
 * When an error is reported via wil (THROW_X, or RETURN_X), wil calls ExecutionContext::CollectError()
 * which will save a record of this error with its current scope so it can be properly reported to the user.
 */

class ExecutionContext
{
public:
    ExecutionContext(Context context, FILE* warningsFile = nullptr) noexcept;
    virtual ~ExecutionContext();

    ExecutionContext(const ExecutionContext&) = delete;
    ExecutionContext(ExecutionContext&&) = delete;

    ExecutionContext& operator=(const ExecutionContext&) = delete;
    ExecutionContext& operator=(ExecutionContext&&) = delete;

    virtual void CollectErrorImpl(HRESULT result);
    virtual bool CanCollectUserErrorMessage();
    bool CanCollectUserWarnings() const;
    void EmitUserWarning(const std::wstring& warning, const std::source_location& location = std::source_location::current());

    void CollectErrorImpl(HRESULT result, ULONGLONG context, std::optional<std::wstring>&& message);

    const std::optional<Error>& ReportedError() const noexcept;

    void SetErrorStringImpl(std::wstring&& string);

    ULONGLONG CurrentContext() const noexcept;

    static void CollectError(HRESULT error);
    static bool ShouldCollectErrorMessage();
    static ExecutionContext* Current();

protected:
    ExecutionContext& RootContext();
    virtual bool CollectUserWarning(const std::wstring& warning);
    std::optional<Error> m_error;
    FILE* m_warningsFile = nullptr;

private:
    ExecutionContext* m_parent = nullptr;
    Context m_context = Context::Empty;
    std::optional<std::wstring> m_errorString;
};

class ClientExecutionContext : public ExecutionContext
{
public:
    ClientExecutionContext(bool enableContextualizedErrors = true);
    ~ClientExecutionContext() override;

    ClientExecutionContext(const ClientExecutionContext&) = delete;
    ClientExecutionContext(ClientExecutionContext&&) = delete;

    ClientExecutionContext& operator=(const ClientExecutionContext&) = delete;
    ClientExecutionContext& operator=(ClientExecutionContext&&) = delete;

    void CollectErrorImpl(HRESULT result) override;

    void FlushWarnings();
    void EnableInteractiveWarnings();

    LXSS_ERROR_INFO* OutError() noexcept;

private:
    LXSS_ERROR_INFO m_outError = {};

    wil::unique_handle m_warningsPipeWrite;
    std::thread m_interactiveWarningsThread;
};

class ServiceExecutionContext : public ExecutionContext
{
public:
    ServiceExecutionContext(LXSS_ERROR_INFO* outError) noexcept;
    ~ServiceExecutionContext() override;

    ServiceExecutionContext(const ServiceExecutionContext&) = delete;
    ServiceExecutionContext(ServiceExecutionContext&&) = delete;

    ServiceExecutionContext& operator=(const ServiceExecutionContext&) = delete;
    ServiceExecutionContext& operator=(ServiceExecutionContext&&) = delete;

    bool CanCollectUserErrorMessage() override;

protected:
    virtual bool CollectUserWarning(const std::wstring& warning) override;

private:
    std::optional<LXSS_ERROR_INFO*> m_outError;
    std::optional<std::wstring> m_warningsString;
    wil::unique_handle m_warningsPipe;
};

void EnableContextualizedErrors(bool service);

void SetErrorMessage(std::wstring&& message);

void SetEventLog(HANDLE eventLog);

} // namespace wsl::windows::common