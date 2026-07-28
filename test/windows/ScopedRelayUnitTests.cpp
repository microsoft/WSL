/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ScopedRelayUnitTests.cpp

Abstract:

    Unit tests for bounded relay::ScopedRelay::Sync().

--*/

#include "precomp.h"
#include "Common.h"

#include <chrono>
#include <string>
#include <vector>
#include "relay.hpp"

namespace ScopedRelayUnitTests {

using namespace std::chrono_literals;
using wsl::windows::common::relay::ScopedRelay;

namespace {

    // Create an overlapped (async) named pipe pair. The returned 'ReadEnd' is the server end
    // opened FILE_FLAG_OVERLAPPED so that a pending ReadFile returns ERROR_IO_PENDING and the
    // relay's InterruptableWait can be cancelled via the exit event -- this mirrors a real
    // guest-owned hvsocket. 'WriteEnd' is the client end used to feed data (and to control EOF
    // by closing it, or to induce a hang by leaving it open).
    struct OverlappedPipe
    {
        wil::unique_handle ReadEnd;
        wil::unique_handle WriteEnd;
    };

    OverlappedPipe CreateOverlappedPipe()
    {
        GUID guid{};
        THROW_IF_FAILED(CoCreateGuid(&guid));

        wchar_t name[64];
        swprintf_s(name, L"\\\\.\\pipe\\wsl-relay-test-%08x%04x%04x", guid.Data1, guid.Data2, guid.Data3);

        OverlappedPipe pipe;
        pipe.ReadEnd.reset(CreateNamedPipeW(
            name,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            64 * 1024,
            64 * 1024,
            0,
            nullptr));
        THROW_LAST_ERROR_IF(!pipe.ReadEnd);

        pipe.WriteEnd.reset(CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        THROW_LAST_ERROR_IF(!pipe.WriteEnd);

        // Complete the named-pipe handshake before starting overlapped reads.
        wil::unique_event connectEvent{wil::EventOptions::ManualReset};
        OVERLAPPED connectOverlapped{};
        connectOverlapped.hEvent = connectEvent.get();
        if (!ConnectNamedPipe(pipe.ReadEnd.get(), &connectOverlapped))
        {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                DWORD transferred = 0;
                THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(pipe.ReadEnd.get(), &connectOverlapped, &transferred, TRUE));
            }
            else
            {
                THROW_HR_IF(HRESULT_FROM_WIN32(error), error != ERROR_PIPE_CONNECTED);
            }
        }

        return pipe;
    }

    // Create a temporary output file (overlapped, delete-on-close) that the relay writes into.
    // The test keeps ownership so it can read the bytes back after Sync().
    wil::unique_handle CreateTempOutputFile()
    {
        wchar_t tempPath[MAX_PATH];
        THROW_LAST_ERROR_IF(GetTempPathW(ARRAYSIZE(tempPath), tempPath) == 0);

        wchar_t tempFile[MAX_PATH];
        THROW_LAST_ERROR_IF(GetTempFileNameW(tempPath, L"rly", 0, tempFile) == 0);

        wil::unique_handle file{CreateFileW(
            tempFile, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
        THROW_LAST_ERROR_IF(!file);

        return file;
    }

    // Read the full contents of an (overlapped) file handle from offset 0 using a synchronous
    // overlapped read, so the relayed payload can be verified deterministically.
    std::vector<char> ReadAllFromStart(HANDLE File, size_t Size)
    {
        std::vector<char> data(Size);
        if (Size == 0)
        {
            return data;
        }

        wil::unique_event event{wil::EventOptions::ManualReset};
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();

        DWORD bytesRead = 0;
        if (!ReadFile(File, data.data(), gsl::narrow_cast<DWORD>(Size), &bytesRead, &overlapped))
        {
            THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
            THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(File, &overlapped, &bytesRead, TRUE));
        }

        data.resize(bytesRead);
        return data;
    }

} // namespace

class ScopedRelayUnitTests
{
    WSL_TEST_CLASS(ScopedRelayUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // Open input without EOF: bounded Sync() must return on timeout.
    TEST_METHOD(SyncTimeoutReturnsWhenInputNeverReachesEof)
    {
        auto pipe = CreateOverlappedPipe();
        auto output = CreateTempOutputFile();

        // Keep the write end open so the relay read stays pending.
        constexpr auto timeout = 500ms;
        constexpr auto budget = 5000ms;

        ScopedRelay relay{pipe.ReadEnd.get(), output.get()};

        const auto start = std::chrono::steady_clock::now();
        relay.Sync(timeout);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

        LogInfo("Sync(%lldms) returned after %lldms", static_cast<long long>(timeout.count()), static_cast<long long>(elapsed.count()));

        VERIFY_IS_TRUE(elapsed < budget);

        // Verify Sync returned due to timeout, not natural EOF.
        VERIFY_IS_TRUE(elapsed >= (timeout - 100ms));
    }

    // Natural EOF should drain the full payload.
    TEST_METHOD(SyncDrainsFullPayloadWithoutTruncation)
    {
        auto pipe = CreateOverlappedPipe();
        auto output = CreateTempOutputFile();

        // Deterministic, verifiable payload (100000 bytes with a non-trivial pattern).
        constexpr size_t payloadSize = 100000;
        std::vector<char> payload(payloadSize);
        for (size_t i = 0; i < payloadSize; ++i)
        {
            payload[i] = static_cast<char>((i * 31 + 7) & 0xff);
        }

        ScopedRelay relay{pipe.ReadEnd.get(), output.get()};

        // Write asynchronously so the relay can drain as the pipe fills.
        std::thread writer{[&]() {
            size_t written = 0;
            while (written < payloadSize)
            {
                DWORD chunk = 0;
                if (!WriteFile(pipe.WriteEnd.get(), payload.data() + written, gsl::narrow_cast<DWORD>(payloadSize - written), &chunk, nullptr))
                {
                    break;
                }

                written += chunk;
            }

            // Natural EOF: closing the write end lets the relay read 0 bytes and complete.
            pipe.WriteEnd.reset();
        }};

        relay.Sync(wsl::windows::common::relay::c_relayDrainTimeout);

        if (writer.joinable())
        {
            writer.join();
        }

        const auto relayed = ReadAllFromStart(output.get(), payloadSize);

        VERIFY_ARE_EQUAL(relayed.size(), payloadSize);
        VERIFY_IS_TRUE(relayed == payload);
    }
};

} // namespace ScopedRelayUnitTests
