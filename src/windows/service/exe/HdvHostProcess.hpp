// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

// VM-owned HDV process. A high-integrity VM worker cannot
// open the system-integrity service for VM_OPERATION/VM_WRITE/DUP_HANDLE.
// Keep the service security unchanged and host the device in a bounded child.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <wil/resource.h>
#include <wil/result.h>

#include "HdvPciDeviceHost.hpp"
#include "WindowsCaptureSource.hpp"

namespace wsl::virtio_sound::process_host {
constexpr std::uint32_t c_startupMagic = 0x48565341;
constexpr std::uint32_t c_startupVersion = 1;

struct ReadyPacket
{
    HRESULT m_result{E_FAIL};
    char m_detail[384]{};
};

struct StartupPacket
{
    std::uint32_t m_magic{c_startupMagic};
    std::uint32_t m_version{c_startupVersion};
    std::uint64_t m_userToken{};
    wchar_t m_endpoint[1025]{};
};

inline HANDLE ParseHandle(const wchar_t* text)
{
    wchar_t* end{};
    const auto value = _wcstoui64(text, &end, 10);
    THROW_HR_IF(E_INVALIDARG, !value || !end || *end);
    return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(value));
}

inline void RequireSystemHighToken()
{
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    const auto user = wil::get_token_information<TOKEN_USER>(token.get());
    THROW_HR_IF(E_ACCESSDENIED, !IsWellKnownSid(user->User.Sid, WinLocalSystemSid));
    const auto label = wil::get_token_information<TOKEN_MANDATORY_LABEL>(token.get());
    const auto sid = label->Label.Sid;
    THROW_HR_IF(E_ACCESSDENIED, *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) != SECURITY_MANDATORY_HIGH_RID);
}

inline int Run(int argc, wchar_t** argv) noexcept
{
    ReadyPacket packet;
    wil::unique_handle output;
    try
    {
        THROW_HR_IF(E_INVALIDARG, argc != 8);
        output.reset(ParseHandle(argv[4]));
        RequireSystemHighToken();
        const auto stop = ParseHandle(argv[5]);
        const auto parent = ParseHandle(argv[6]);
        wil::unique_handle configuration{ParseHandle(argv[7])};
        DWORD flags{};
        for (const auto handle : {output.get(), stop, parent, configuration.get()})
        {
            THROW_IF_WIN32_BOOL_FALSE(GetHandleInformation(handle, &flags));
            THROW_HR_IF(E_ACCESSDENIED, !(flags & HANDLE_FLAG_INHERIT));
        }
        THROW_HR_IF(E_INVALIDARG, GetFileType(output.get()) != FILE_TYPE_PIPE);
        THROW_HR_IF(E_INVALIDARG, GetFileType(configuration.get()) != FILE_TYPE_PIPE);
        StartupPacket startup;
        DWORD received{};
        THROW_IF_WIN32_BOOL_FALSE(ReadFile(configuration.get(), &startup, sizeof(startup), &received, nullptr));
        configuration.reset();
        THROW_HR_IF(E_INVALIDARG, received != sizeof(startup) || startup.m_magic != c_startupMagic || startup.m_version != c_startupVersion);
        THROW_HR_IF(E_INVALIDARG, startup.m_endpoint[ARRAYSIZE(startup.m_endpoint) - 1] != 0);
        THROW_HR_IF(E_INVALIDARG, (startup.m_userToken != 0) != (startup.m_endpoint[0] != 0));
        GUID vm{}, instance{};
        // HCS names use the unbraced GUID form; CLSIDFromString requires braces.
        const std::wstring vmText = argv[2];
        THROW_HR_IF(E_INVALIDARG, vmText.size() != 36);
        const std::wstring bracedVm = L"{" + vmText + L"}";
        THROW_IF_FAILED(CLSIDFromString(bracedVm.c_str(), &vm));
        THROW_IF_FAILED(CLSIDFromString(argv[3], &instance));
        THROW_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        auto uninitialize = wil::scope_exit([] { CoUninitialize(); });
        using unique_hcs_system = wil::unique_any<HCS_SYSTEM, decltype(&HcsCloseComputeSystem), HcsCloseComputeSystem>;
        unique_hcs_system system;
        THROW_IF_FAILED(HcsOpenComputeSystem(argv[2], GENERIC_ALL, &system));
        std::shared_ptr<native::windows::WindowsCaptureSource> source;
        auto shutdownSource = wil::scope_exit([&] {
            if (source)
            {
                source->Shutdown();
            }
        });
        if (startup.m_userToken)
        {
            wil::unique_handle userToken{reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(startup.m_userToken))};
            THROW_IF_WIN32_BOOL_FALSE(GetHandleInformation(userToken.get(), &flags));
            THROW_HR_IF(E_ACCESSDENIED, !(flags & HANDLE_FLAG_INHERIT));
            // The WASAPI worker stays in the original restricted user session.
            // Create authenticates an idle worker; only a guest START opens audio.
            wchar_t image[32768]{};
            const auto length = GetModuleFileNameW(nullptr, image, ARRAYSIZE(image));
            THROW_LAST_ERROR_IF(!length || length == ARRAYSIZE(image));
            const auto worker = std::filesystem::path(image).parent_path() / L"wsl-audio-worker.exe";
            source = native::windows::WindowsCaptureSource::Create(userToken.get(), worker.wstring(), startup.m_endpoint);
        }
        constexpr GUID c_deviceClass{0xF1919B41, 0x6A33, 0x4C92, {0xA7, 0xF8, 0x39, 0x7E, 0xE4, 0xC5, 0xB1, 0xD2}};
        auto host = native::windows::HdvPciDeviceHost::CreateBorrowed(
            system.get(), c_deviceClass, instance, source, native::windows::HostOwnership::UnknownOrShared);
        packet.m_result = S_OK;
        DWORD written{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(output.get(), &packet, sizeof(packet), &written, nullptr));
        THROW_HR_IF(E_FAIL, written != sizeof(packet));
        output.reset();
        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        const HANDLE waits[]{stop, parent};
        for (;;)
        {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 10);
            if (wait == WAIT_OBJECT_0)
            {
                // The parent signals stop only after confirming guest shutdown.
                break;
            }
            if (wait == WAIT_OBJECT_0 + 1)
            {
                // Parent death does not prove that the guest stopped. Close the
                // capture source during unwinding and let process exit release
                // the process-pinned callback root; never claim guest retirement.
                if (source)
                {
                    source->Shutdown();
                }
                shutdownSource.release();
                return ERROR_PROCESS_ABORTED;
            }
            if (wait == WAIT_FAILED)
            {
                // Fail closed: unwinding shuts down the source, but leaves the
                // callback root pinned until this child process exits.
                THROW_LAST_ERROR();
            }
            if (wait != WAIT_TIMEOUT)
            {
                THROW_HR(E_UNEXPECTED);
            }

            const auto now = clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count();
            last = now;
            if (elapsed > 0)
            {
                host->PumpCaptureElapsed(std::min<std::uint64_t>(static_cast<std::uint64_t>(elapsed), 20000000));
            }
        }
        // The parent confirms guest shutdown before signaling stop. Keep the
        // existing ownership rule: do not tear down another WSL proxy registration.
        (void)host->GuestStoppedAndRetire();
        return 0;
    }
    catch (const std::exception& error)
    {
        strncpy_s(packet.m_detail, error.what(), _TRUNCATE);
    }
    catch (...)
    {
        packet.m_result = wil::ResultFromCaughtException();
    }
    if (output)
    {
        DWORD written{};
        (void)WriteFile(output.get(), &packet, sizeof(packet), &written, nullptr);
        output.reset();
    }
    return 1;
}

class HdvHostProcess final
{
public:
    static std::unique_ptr<HdvHostProcess> Create(
        const std::wstring& vm, const GUID& instance, HANDLE restrictedUserToken = nullptr, const std::wstring& endpoint = {})
    {
        auto host = std::make_unique<HdvHostProcess>();
        THROW_HR_IF(E_INVALIDARG, (restrictedUserToken != nullptr) != !endpoint.empty());
        StartupPacket startupPacket;
        THROW_HR_IF(E_INVALIDARG, endpoint.size() >= ARRAYSIZE(startupPacket.m_endpoint) || endpoint.find(L'\0') != std::wstring::npos);
        std::copy(endpoint.begin(), endpoint.end(), startupPacket.m_endpoint);
        wil::unique_handle processToken, childToken;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken));
        THROW_IF_WIN32_BOOL_FALSE(
            DuplicateTokenEx(processToken.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &childToken));
        const auto user = wil::get_token_information<TOKEN_USER>(childToken.get());
        THROW_HR_IF(E_ACCESSDENIED, !IsWellKnownSid(user->User.Sid, WinLocalSystemSid));
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
        PSID highSid{};
        THROW_IF_WIN32_BOOL_FALSE(AllocateAndInitializeSid(&authority, 1, SECURITY_MANDATORY_HIGH_RID, 0, 0, 0, 0, 0, 0, 0, &highSid));
        auto freeSid = wil::scope_exit([&] { FreeSid(highSid); });
        TOKEN_MANDATORY_LABEL label{{highSid, SE_GROUP_INTEGRITY}};
        THROW_IF_WIN32_BOOL_FALSE(SetTokenInformation(childToken.get(), TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(highSid)));

        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        wil::unique_handle read, write, parent;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&read, &write, &attributes, 0));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0));
        host->m_stop.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        THROW_LAST_ERROR_IF(!host->m_stop);
        THROW_IF_WIN32_BOOL_FALSE(
            DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent, SYNCHRONIZE, TRUE, 0));
        wil::unique_handle configurationRead, configurationWrite, inheritedUserToken;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&configurationRead, &configurationWrite, &attributes, sizeof(StartupPacket)));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(configurationWrite.get(), HANDLE_FLAG_INHERIT, 0));
        std::vector<HANDLE> inherited{write.get(), host->m_stop.get(), parent.get(), configurationRead.get()};
        if (restrictedUserToken)
        {
            THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
                GetCurrentProcess(), restrictedUserToken, GetCurrentProcess(), &inheritedUserToken, TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, 0));
            inherited.push_back(inheritedUserToken.get());
            startupPacket.m_userToken = reinterpret_cast<ULONG_PTR>(inheritedUserToken.get());
        }
        DWORD sent{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(configurationWrite.get(), &startupPacket, sizeof(startupPacket), &sent, nullptr));
        THROW_HR_IF(E_FAIL, sent != sizeof(startupPacket));
        configurationWrite.reset();
        host->m_job = wsl::windows::common::helpers::CreateKillOnCloseJob();
        if (restrictedUserToken)
        {
            // The session-0 host and user-session WASAPI worker cannot share a
            // Windows job. Permit explicit breakaway only for this dedicated
            // host; the worker launch atomically assigns its own kill-on-close
            // job, whose last handle is owned by the host. Never silently break
            // arbitrary descendants away from lifecycle management.
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
            THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(host->m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
        }
        HANDLE jobHandle = host->m_job.get();
        auto processAttributes = wsl::windows::common::helpers::CreateProcThreadAttributeList(2);
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            processAttributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr));
        // Keep jobHandle alive until after the attribute list is destroyed.
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            processAttributes.get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &jobHandle, sizeof(jobHandle), nullptr, nullptr));
        wchar_t image[32768]{};
        const auto length = GetModuleFileNameW(nullptr, image, ARRAYSIZE(image));
        THROW_LAST_ERROR_IF(!length || length == ARRAYSIZE(image));
        wchar_t instanceText[40]{};
        THROW_HR_IF(E_FAIL, !StringFromGUID2(instance, instanceText, ARRAYSIZE(instanceText)));
        auto handleText = [](HANDLE h) { return std::to_wstring(reinterpret_cast<ULONG_PTR>(h)); };
        std::wstring command = L"\"" + std::wstring(image) + L"\" --virtio-sound-host " + vm + L" " + instanceText + L" " +
                               handleText(write.get()) + L" " + handleText(host->m_stop.get()) + L" " + handleText(parent.get()) +
                               L" " + handleText(configurationRead.get());
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = processAttributes.get();
        PROCESS_INFORMATION process{};
        THROW_IF_WIN32_BOOL_FALSE(CreateProcessAsUserW(
            childToken.get(),
            image,
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process));
        host->m_child.reset(process.hProcess);
        wil::unique_handle thread{process.hThread};
        auto killOnFailure = wil::scope_exit([&] { TerminateProcess(host->m_child.get(), 1); });
        // PROC_THREAD_ATTRIBUTE_JOB_LIST assigns the suspended process before
        // CreateProcessAsUserW returns. Verify the expected job before allowing
        // the child to execute; a failure here cannot leave an unbounded host.
        BOOL inHostJob = FALSE;
        THROW_IF_WIN32_BOOL_FALSE(IsProcessInJob(host->m_child.get(), host->m_job.get(), &inHostJob));
        THROW_HR_IF(E_UNEXPECTED, !inHostJob);
        THROW_LAST_ERROR_IF(ResumeThread(thread.get()) == static_cast<DWORD>(-1));
        write.reset();
        configurationRead.reset();
        inheritedUserToken.reset();
        const auto deadline = GetTickCount64() + 30000;
        ReadyPacket packet;
        for (;;)
        {
            DWORD available{};
            THROW_IF_WIN32_BOOL_FALSE(PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr));
            if (available >= sizeof(packet))
            {
                break;
            }
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), GetTickCount64() >= deadline);
            if (WaitForSingleObject(host->m_child.get(), 20) != WAIT_TIMEOUT)
            {
                // A fast child failure may write its diagnostic and exit between
                // PeekNamedPipe and the wait; consume that final packet first.
                THROW_IF_WIN32_BOOL_FALSE(PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr));
                if (available >= sizeof(packet))
                {
                    break;
                }
                DWORD exitCode{};
                THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(host->m_child.get(), &exitCode));
                THROW_HR_MSG(E_FAIL, "HDV child exited before readiness, code=0x%08x", exitCode);
            }
        }
        DWORD received{};
        THROW_IF_WIN32_BOOL_FALSE(ReadFile(read.get(), &packet, sizeof(packet), &received, nullptr));
        THROW_HR_IF(E_FAIL, received != sizeof(packet));
        packet.m_detail[sizeof(packet.m_detail) - 1] = 0;
        THROW_HR_IF_MSG(packet.m_result, packet.m_result < 0, "HDV child: %hs", packet.m_detail);
        killOnFailure.release();
        return host;
    }

    // Must be called only after the parent has observed confirmed guest exit.
    // Without this confirmation, destruction uses the job as an emergency
    // abort and the child skips GuestStoppedAndRetire.
    void ConfirmGuestStopped() noexcept
    {
        m_guestStoppedConfirmed.store(true, std::memory_order_release);
    }

    ~HdvHostProcess()
    {
        if (!m_guestStoppedConfirmed.load(std::memory_order_acquire))
        {
            // Emergency abort: there is no proof that the guest stopped, so do
            // not ask the child to claim retirement. The callback root remains
            // process-pinned until job closure terminates the child.
            m_job.reset();
            return;
        }

        if (!m_stop || !SetEvent(m_stop.get()))
        {
            // A failed graceful signal falls back to the bounded job abort.
            m_job.reset();
            return;
        }

        if (m_child && WaitForSingleObject(m_child.get(), 10000) != WAIT_OBJECT_0)
        {
            // The guest did stop, but the HDV child did not retire in time.
            // Closing the kill-on-close job is the explicit emergency fallback.
            m_job.reset();
        }
    }

private:
    wil::unique_handle m_job;
    wil::unique_handle m_stop;
    wil::unique_handle m_child;
    std::atomic_bool m_guestStoppedConfirmed{};
};
} // namespace wsl::virtio_sound::process_host
