#pragma once

#include "MainWindow.g.h"
#include "Subtitler.h"
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <memory>

namespace winrt::WSLAMoviePlayer::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        // Event handlers for UI
        void OpenFileButton_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void PlayPauseButton_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void SkipBackButton_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void SkipForwardButton_Click(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void ProgressSlider_ValueChanged(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void ProgressSlider_PointerPressed(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void ProgressSlider_PointerReleased(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

    private:
        enum class PlaybackState {
            UserPaused,
            Playing,
            Buffering
        };

        // Media player
        Windows::Media::Playback::MediaPlayer m_mediaPlayer{ nullptr };
        
        // Subtitler
        std::unique_ptr<Subtitler> m_subtitler;
        
        // State
        int64_t m_duration;  // in milliseconds
        PlaybackState m_playbackState;
        bool m_isSliderDragging;
        
        // Event tokens for cleanup
        winrt::event_token m_positionChangedToken;
        winrt::event_token m_mediaOpenedToken;
        winrt::event_token m_mediaFailedToken;
        winrt::event_token m_playbackStateChangedToken;

        // Helper methods
        void SetupMediaPlayer();
        void SetupConnections();
        winrt::Windows::Foundation::IAsyncAction OpenFileAsync();
        void PlayPause();
        void SkipForward();
        void SkipBackward();
        void UpdatePosition(Windows::Foundation::TimeSpan position);
        void UpdateDuration(Windows::Foundation::TimeSpan duration);
        void Seek(int64_t positionMs);
        void UpdatePlayPauseButton();
        void UpdateCoverageOverlay();
        winrt::hstring FormatTime(int64_t milliseconds);
        
        // Subtitler callbacks
        void OnNativeSubtitleChanged(const winrt::hstring& subtitle);
        void OnTranslatedSubtitleChanged(const winrt::hstring& subtitle);
        void OnProcessingStarted(double seekTime);
        void OnProcessingFinished();
        void OnSubtitlesUpdated();
        void OnConnectionEstablished();
        void OnConnectionLost();
        void OnConnectionError(const winrt::hstring& error);
        
        // Dispatcher for UI thread
        Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{ nullptr };
    };
}

namespace winrt::WSLAMoviePlayer::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
