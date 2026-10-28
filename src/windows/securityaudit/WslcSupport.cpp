// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WslcSupport.h"

namespace wsl::audit {

std::optional<WslcApi> GetWslcApi(const WSLPluginAPIV1& api)
{
    const auto& version = api.Version;
    if (version.Major < 2 || (version.Major == 2 && (version.Minor < 9 || (version.Minor == 9 && version.Revision < 5))))
    {
        return {};
    }
    WslcApi result{
        api.WSLCMountFolder, api.WSLCUnmountFolder, api.WSLCCreateProcess, api.WSLCProcessGetFd, api.WSLCProcessGetExitEvent, api.WSLCProcessGetExitCode, api.WSLCReleaseProcess};
    THROW_HR_IF_MSG(
        E_NOINTERFACE,
        !result.mount || !result.unmount || !result.createProcess || !result.getFd || !result.getExitEvent ||
            !result.getExitCode || !result.releaseProcess,
        "WSL advertises WSLC VM hooks but does not supply the required process APIs.");
    return result;
}

ContainerIdentity ReadContainerIdentity(std::string_view inspect)
{
    THROW_HR_IF_MSG(E_INVALIDARG, inspect.empty() || inspect.size() > 1024 * 1024, "Invalid WSLC container notification size.");
    const auto document = nlohmann::json::parse(inspect);
    const auto text = [&](const nlohmann::json& object, const char* name, size_t maximum) {
        const auto& value = object.at(name);
        THROW_HR_IF(E_INVALIDARG, !value.is_string());
        const auto& result = value.get_ref<const std::string&>();
        THROW_HR_IF(E_INVALIDARG, result.empty() || result.size() > maximum || result.find('\0') != std::string::npos);
        ToWide(result);
        return result;
    };
    ContainerIdentity result;
    result.id = text(document, "Id", 128);
    result.name = text(document, "Name", 256);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        !document.contains("WslPluginContext"),
        "WSLC container attribution requires a host with WslPluginContext metadata.");
    const auto& context = document.at("WslPluginContext");
    THROW_HR_IF(E_INVALIDARG, !context.is_object() || !context.at("Version").is_number_unsigned() || context.at("Version") != 1);
    const auto& pid = context.at("InitPid");
    THROW_HR_IF(E_INVALIDARG, !pid.is_number_unsigned() || pid.get<uint64_t>() > INT32_MAX);
    result.initPid = pid.get<uint32_t>();
    result.startedAt = text(context, "StartedAt", 128);
    return result;
}

std::vector<LPCSTR> CollectorArguments(LPCSTR option, LPCSTR value)
{
    return {
#if defined(_ARM64_)
        "/wsl-security-audit/runtime/ld-linux-aarch64.so.1",
#else
        "/wsl-security-audit/runtime/ld-linux-x86-64.so.2",
#endif
        "--inhibit-cache",
        "--library-path",
        "/wsl-security-audit/runtime",
        "/wsl-security-audit/wsl_audit_collector",
        option,
        value,
        nullptr};
}

WslcProcess::WslcProcess(WslcApi api) : m_api(api), m_process(nullptr, api.releaseProcess)
{
    THROW_HR_IF(E_INVALIDARG, !api.createProcess || !api.getFd || !api.getExitEvent || !api.getExitCode || !api.releaseProcess);
}

void WslcProcess::Start(WSLCSessionId session, std::vector<LPCSTR> arguments)
{
    THROW_HR_IF(E_UNEXPECTED, m_process != nullptr);
    WSLCProcessHandle process{};
    int error{};
    const auto result = m_api.createProcess(session, arguments.at(0), arguments.data(), nullptr, &process, &error);
    m_process.reset(process);
    THROW_IF_FAILED_MSG(result, "Cannot create WSLC audit process (Linux errno %d).", error);
    THROW_HR_IF(E_HANDLE, !m_process);
    const auto acquire = [&](WSLCProcessFd fd, UniqueSocket& owner) {
        SOCKET socket = INVALID_SOCKET;
        const auto code = m_api.getFd(m_process.get(), fd, &socket);
        owner.reset(socket);
        THROW_IF_FAILED(code);
        THROW_HR_IF(E_HANDLE, !owner);
    };
    acquire(WSLCProcessFdStdin, m_input);
    acquire(WSLCProcessFdStdout, m_output);
    acquire(WSLCProcessFdStderr, m_error);
    HANDLE event{};
    const auto code = m_api.getExitEvent(m_process.get(), &event);
    m_exitEvent.reset(event);
    THROW_IF_FAILED(code);
    THROW_HR_IF(E_HANDLE, !m_exitEvent);
}

int WslcProcess::WaitForExit(DWORD milliseconds) const
{
    const auto wait = WaitForSingleObject(m_exitEvent.get(), milliseconds);
    THROW_LAST_ERROR_IF(wait == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), wait != WAIT_OBJECT_0);
    int code{};
    THROW_IF_FAILED(m_api.getExitCode(m_process.get(), &code));
    return code;
}

void WslcProcess::Shutdown() const noexcept
{
    for (const auto socket : {Input(), Output(), Error()})
    {
        if (socket != INVALID_SOCKET && shutdown(socket, SD_BOTH) == SOCKET_ERROR)
        {
            const auto error = WSAGetLastError();
            if (error != WSAENOTCONN)
            {
                LOG_IF_FAILED(HRESULT_FROM_WIN32(error));
            }
        }
    }
}

void ResolveContainerNamespaces(const WslcApi& api, WSLCSessionId session, ContainerIdentity& container)
{
    THROW_HR_IF(E_INVALIDARG, container.initPid == 0);
    WslcProcess probe(api);
    const auto pid = std::to_string(container.initPid);
    auto arguments = CollectorArguments("--inspect-pid", pid.c_str());
    arguments.pop_back();
    arguments.insert(arguments.end(), {"--container-id", container.id.c_str(), nullptr});
    probe.Start(session, std::move(arguments));
    const auto cleanup = wil::scope_exit([&] { probe.Shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string output;
    bool finished{};
    bool stderrOpen{true};
    while (!finished)
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), std::chrono::steady_clock::now() >= deadline);
        fd_set readable{};
        FD_ZERO(&readable);
        FD_SET(probe.Output(), &readable);
        if (stderrOpen)
        {
            FD_SET(probe.Error(), &readable);
        }
        timeval timeout{0, 100000};
        const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
        THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), ready == SOCKET_ERROR);
        std::array<char, 2048> bytes{};
        if (stderrOpen && FD_ISSET(probe.Error(), &readable))
        {
            const auto count = recv(probe.Error(), bytes.data(), static_cast<int>(bytes.size()), 0);
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), count == SOCKET_ERROR);
            stderrOpen = count != 0;
            if (count > 0)
            {
                OutputDebugStringW(L"WSL Security audit: namespace inspection emitted stderr diagnostics.\n");
            }
        }
        if (FD_ISSET(probe.Output(), &readable))
        {
            const auto count = recv(probe.Output(), bytes.data(), static_cast<int>(bytes.size()), 0);
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), count == SOCKET_ERROR);
            finished = count == 0;
            THROW_HR_IF(E_INVALIDARG, output.size() + count > 4096);
            output.append(bytes.data(), count);
        }
    }
    THROW_HR_IF_MSG(E_FAIL, probe.WaitForExit(1000) != 0, "WSLC container namespace inspection failed.");
    const auto document = nlohmann::json::parse(output);
    THROW_HR_IF(
        E_INVALIDARG,
        !document.is_object() || document.size() != 4 || !document.at("pid").is_number_unsigned() ||
            document.at("pid") != container.initPid);
    const auto read = [&](const char* name) {
        const auto& value = document.at(name);
        THROW_HR_IF(E_INVALIDARG, !value.is_number_unsigned() || value == 0 || value.get<uint64_t>() > UINT32_MAX);
        return value.get<uint64_t>();
    };
    const auto pidNamespace = read("pidNamespace");
    const auto mountNamespace = read("mountNamespace");
    const auto& observed = document.at("observedAfterNs");
    THROW_HR_IF(E_INVALIDARG, !observed.is_number_unsigned() || observed == 0);
    container.pidNamespace = pidNamespace;
    container.mountNamespace = mountNamespace;
    container.observedAfterNs = observed.get<uint64_t>();
}

} // namespace wsl::audit
