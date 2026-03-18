#pragma once

#include "pch.h"
#include "wslcsdk.h"
#include <string>
#include <functional>

namespace winrt::WSLAMoviePlayer::implementation
{
    class Container
    {
    public:
        Container();
        ~Container();

        winrt::Windows::Foundation::IAsyncAction StartAsync();
        void Stop();
        bool IsRunning() const;

        // Event callbacks (following Socket pattern)
        std::function<void()> OnContainerStarted;
        std::function<void()> OnContainerStopped;
        std::function<void(const winrt::hstring&)> OnContainerError;
        std::function<void(const winrt::hstring&)> OnContainerOutput;
        std::function<void(const winrt::hstring&)> OnContainerLog;

    private:
        bool m_isRunning;

        // WSL Container API handles (raw pointers managed manually)
        WslcSession m_session;
        WslcContainer m_container;
        WslcProcess m_process;

        void Cleanup();
        void Log(const std::wstring& msg);
    };
}
