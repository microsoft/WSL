#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.h>
#include <chrono>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Windows::Foundation;
using namespace Windows::Media::Playback;
using namespace Windows::Media::Core;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Microsoft::UI::Xaml::Shapes;
using namespace Microsoft::UI::Xaml::Media;

namespace winrt::WSLAMoviePlayer::implementation
{
    MainWindow::MainWindow()
        : m_duration(0)
        , m_playbackState(PlaybackState::UserPaused)
        , m_isSliderDragging(false)
    {
        InitializeComponent();
        
        // Get dispatcher queue for UI thread updates
        m_dispatcherQueue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        
        SetupMediaPlayer();
        
        // Create subtitler
        m_subtitler = std::make_unique<Subtitler>();
        SetupConnections();
        
        // Initialize subtitler connection (fire-and-forget)
        auto initAsync = m_subtitler->InitializeAsync();
        
        // Create and start container (fire-and-forget)
        m_container = std::make_unique<Container>();
        SetupContainerCallbacks();
        auto containerAsync = m_container->StartAsync();
    }

    MainWindow::~MainWindow()
    {
        // Stop container
        if (m_container)
        {
            m_container->Stop();
        }
        
        // Clean up event handlers
        if (m_mediaPlayer)
        {
            m_mediaPlayer.PlaybackSession().PositionChanged(m_positionChangedToken);
            m_mediaPlayer.MediaOpened(m_mediaOpenedToken);
            m_mediaPlayer.MediaFailed(m_mediaFailedToken);
            m_mediaPlayer.PlaybackSession().PlaybackStateChanged(m_playbackStateChangedToken);
        }
    }

    void MainWindow::SetupMediaPlayer()
    {
        m_mediaPlayer = MediaPlayer();
        MediaPlayerElement().SetMediaPlayer(m_mediaPlayer);
        
        // Position changed event
        m_positionChangedToken = m_mediaPlayer.PlaybackSession().PositionChanged(
            [this](MediaPlaybackSession const& session, IInspectable const&)
            {
                auto position = session.Position();
                m_dispatcherQueue.TryEnqueue([this, position]()
                {
                    UpdatePosition(position);
                });
            });
        
        // Media opened event
        m_mediaOpenedToken = m_mediaPlayer.MediaOpened(
            [this](MediaPlayer const& sender, IInspectable const&)
            {
                auto duration = sender.PlaybackSession().NaturalDuration();
                m_dispatcherQueue.TryEnqueue([this, duration]()
                {
                    UpdateDuration(duration);
                });
            });
        
        // Media failed event
        m_mediaFailedToken = m_mediaPlayer.MediaFailed(
            [this](MediaPlayer const&, MediaPlayerFailedEventArgs const& args)
            {
                auto error = args.ErrorMessage();
                m_dispatcherQueue.TryEnqueue([this, error]()
                {
                    ContentDialog dialog;
                    dialog.XamlRoot(this->Content().XamlRoot());
                    dialog.Title(box_value(L"Media Error"));
                    dialog.Content(box_value(L"Failed to load or play the media file.\n\nError: " + error));
                    dialog.CloseButtonText(L"OK");
                    dialog.ShowAsync();
                });
            });
        
        // Playback state changed event
        m_playbackStateChangedToken = m_mediaPlayer.PlaybackSession().PlaybackStateChanged(
            [this](MediaPlaybackSession const&, IInspectable const&)
            {
                m_dispatcherQueue.TryEnqueue([this]()
                {
                    UpdatePlayPauseButton();
                });
            });
    }

    void MainWindow::SetupConnections()
    {
        // Set up subtitler callbacks
        m_subtitler->OnNativeSubtitleChanged = [this](const hstring& subtitle)
        {
            m_dispatcherQueue.TryEnqueue([this, subtitle]()
            {
                OnNativeSubtitleChanged(subtitle);
            });
        };
        
        m_subtitler->OnTranslatedSubtitleChanged = [this](const hstring& subtitle)
        {
            m_dispatcherQueue.TryEnqueue([this, subtitle]()
            {
                OnTranslatedSubtitleChanged(subtitle);
            });
        };
        
        m_subtitler->OnProcessingStarted = [this](double seekTime)
        {
            m_dispatcherQueue.TryEnqueue([this, seekTime]()
            {
                OnProcessingStarted(seekTime);
            });
        };
        
        m_subtitler->OnProcessingFinished = [this]()
        {
            m_dispatcherQueue.TryEnqueue([this]()
            {
                OnProcessingFinished();
            });
        };
        
        m_subtitler->OnSubtitlesUpdated = [this]()
        {
            m_dispatcherQueue.TryEnqueue([this]()
            {
                OnSubtitlesUpdated();
            });
        };
        
        m_subtitler->OnConnectionEstablished = [this]()
        {
            m_dispatcherQueue.TryEnqueue([this]()
            {
                OnConnectionEstablished();
            });
        };
        
        m_subtitler->OnConnectionLost = [this]()
        {
            m_dispatcherQueue.TryEnqueue([this]()
            {
                OnConnectionLost();
            });
        };
        
        m_subtitler->OnConnectionError = [this](const hstring& error)
        {
            m_dispatcherQueue.TryEnqueue([this, error]()
            {
                OnConnectionError(error);
            });
        };
    }

    void MainWindow::OpenFileButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        auto openAsync = OpenFileAsync();
    }

    IAsyncAction MainWindow::OpenFileAsync()
    {
        auto lifetime = get_strong();
        
        FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(GetActiveWindow());
        picker.SuggestedStartLocation(PickerLocationId::VideosLibrary);
        picker.FileTypeFilter().Append(L".mp4");
        picker.FileTypeFilter().Append(L".avi");
        picker.FileTypeFilter().Append(L".mkv");
        picker.FileTypeFilter().Append(L".mov");
        picker.FileTypeFilter().Append(L".wmv");
        
        StorageFile file = co_await picker.PickSingleFileAsync();
        
        if (file)
        {
            OutputDebugStringW((L"MainWindow: Selected file: " + file.Path() + L"\n").c_str());
            
            // Set media source
            auto mediaSource = MediaSource::CreateFromStorageFile(file);
            m_mediaPlayer.Source(mediaSource);
            
            // Enable play button
            PlayPauseButton().IsEnabled(true);
            
            // Start playing
            m_playbackState = PlaybackState::Playing;
            m_mediaPlayer.Play();
            
            // Set video source for subtitler
            co_await m_subtitler->SetVideoSourceAsync(file);
            
            // Update window title
            Title(L"WSLA Movie Player - " + file.Name());
        }
    }

    void MainWindow::PlayPauseButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        PlayPause();
    }

    void MainWindow::SkipBackButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        SkipBackward();
    }

    void MainWindow::SkipForwardButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        SkipForward();
    }

    void MainWindow::ProgressSlider_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        if (m_isSliderDragging && m_duration > 0)
        {
            // Calculate position from slider value (0-100 range)
            int64_t newPosition = static_cast<int64_t>((args.NewValue() / 100.0) * m_duration);
            Seek(newPosition);
        }
    }

    void MainWindow::ProgressSlider_PointerPressed(IInspectable const&, Input::PointerRoutedEventArgs const&)
    {
        m_isSliderDragging = true;
    }

    void MainWindow::ProgressSlider_PointerReleased(IInspectable const&, Input::PointerRoutedEventArgs const&)
    {
        m_isSliderDragging = false;
        
        // Check if there's a subtitle at the current position
        if (m_subtitler && m_duration > 0)
        {
            int64_t position = static_cast<int64_t>((ProgressSlider().Value() / 100.0) * m_duration);
            if (!m_subtitler->HasSubtitleAt(position))
            {
                OutputDebugStringW(L"MainWindow: Slider released at position with no subtitle - requesting from server\n");
                auto seekAsync = m_subtitler->SeekAsync(position);
            }
        }
    }

    void MainWindow::PlayPause()
    {
        if (m_playbackState == PlaybackState::Playing || m_playbackState == PlaybackState::Buffering)
        {
            m_playbackState = PlaybackState::UserPaused;
            m_mediaPlayer.Pause();
        }
        else
        {
            m_playbackState = PlaybackState::Playing;
            m_mediaPlayer.Play();
        }
        UpdatePlayPauseButton();
    }

    void MainWindow::SkipForward()
    {
        auto currentPosition = m_mediaPlayer.PlaybackSession().Position();
        auto newPosition = currentPosition + std::chrono::seconds(15);
        
        if (newPosition > m_mediaPlayer.PlaybackSession().NaturalDuration())
        {
            newPosition = m_mediaPlayer.PlaybackSession().NaturalDuration();
        }
        
        m_mediaPlayer.PlaybackSession().Position(newPosition);
    }

    void MainWindow::SkipBackward()
    {
        auto currentPosition = m_mediaPlayer.PlaybackSession().Position();
        auto newPosition = currentPosition - std::chrono::seconds(15);
        
        if (newPosition.count() < 0)
        {
            newPosition = TimeSpan::zero();
        }
        
        m_mediaPlayer.PlaybackSession().Position(newPosition);
    }

    void MainWindow::UpdatePosition(TimeSpan position)
    {
        auto positionMs = std::chrono::duration_cast<std::chrono::milliseconds>(position).count();
        
        // Update slider if not dragging
        if (!m_isSliderDragging && m_duration > 0)
        {
            double sliderValue = (static_cast<double>(positionMs) / m_duration) * 100.0;
            ProgressSlider().Value(sliderValue);
        }
        
        // Update time label
        TimeLabel().Text(FormatTime(positionMs) + L" / " + FormatTime(m_duration));
        
        // Update subtitler position
        if (m_subtitler)
        {
            m_subtitler->UpdatePosition(positionMs);
        }
    }

    void MainWindow::UpdateDuration(TimeSpan duration)
    {
        m_duration = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        OutputDebugStringW((L"MainWindow: Duration set to " + to_hstring(m_duration) + L" ms\n").c_str());
    }

    void MainWindow::Seek(int64_t positionMs)
    {
        auto newPosition = std::chrono::milliseconds(positionMs);
        m_mediaPlayer.PlaybackSession().Position(newPosition);
    }

    void MainWindow::UpdatePlayPauseButton()
    {
        switch (m_playbackState)
        {
            case PlaybackState::Playing:
                PlayPauseIcon().Symbol(Symbol::Pause);
                PlayPauseLabel().Text(L"Pause");
                break;
            case PlaybackState::Buffering:
                PlayPauseIcon().Symbol(Symbol::Clock);
                PlayPauseLabel().Text(L"Buffering...");
                break;
            default:
                PlayPauseIcon().Symbol(Symbol::Play);
                PlayPauseLabel().Text(L"Play");
                break;
        }
    }

    hstring MainWindow::FormatTime(int64_t milliseconds)
    {
        int64_t seconds = milliseconds / 1000;
        int64_t minutes = seconds / 60;
        seconds = seconds % 60;
        int64_t hours = minutes / 60;
        minutes = minutes % 60;

        wchar_t buffer[32];
        if (hours > 0)
        {
            swprintf_s(buffer, L"%02lld:%02lld:%02lld", hours, minutes, seconds);
        }
        else
        {
            swprintf_s(buffer, L"%02lld:%02lld", minutes, seconds);
        }
        return hstring(buffer);
    }

    void MainWindow::OnNativeSubtitleChanged(const hstring& subtitle)
    {
        NativeSubtitleText().Text(subtitle);
    }

    void MainWindow::OnTranslatedSubtitleChanged(const hstring& subtitle)
    {
        TranslatedSubtitleText().Text(subtitle);
    }

    void MainWindow::OnProcessingStarted(double seekTime)
    {
        OutputDebugStringW((L"MainWindow: Processing started at " + to_hstring(seekTime) + L" seconds\n").c_str());
        ProcessingIndicator().Visibility(Visibility::Visible);
    }

    void MainWindow::OnProcessingFinished()
    {
        OutputDebugStringW(L"MainWindow: Processing finished\n");
        ProcessingIndicator().Visibility(Visibility::Collapsed);
    }

    void MainWindow::OnSubtitlesUpdated()
    {
        OutputDebugStringW(L"MainWindow: Subtitles updated\n");
        UpdateCoverageOverlay();
    }

    void MainWindow::UpdateCoverageOverlay()
    {
        if (!m_subtitler || m_duration <= 0)
            return;

        auto canvas = CoverageCanvas();
        canvas.Children().Clear();

        double canvasWidth = canvas.ActualWidth();
        if (canvasWidth <= 0)
        {
            // Canvas not laid out yet; use the slider width as fallback
            canvasWidth = ProgressSlider().ActualWidth();
            if (canvasWidth <= 0)
                return;
        }

        auto greenBrush = SolidColorBrush(Windows::UI::ColorHelper::FromArgb(180, 0, 180, 0));

        for (const auto& range : m_subtitler->GetCoverageRanges())
        {
            double startFrac = static_cast<double>(range.start) / m_duration;
            double endFrac = static_cast<double>(range.end) / m_duration;

            double x = startFrac * canvasWidth;
            double width = (endFrac - startFrac) * canvasWidth;
            if (width < 1.0) width = 1.0;

            Microsoft::UI::Xaml::Shapes::Rectangle rect;
            rect.Fill(greenBrush);
            rect.Width(width);
            rect.Height(4);
            rect.RadiusX(2);
            rect.RadiusY(2);
            Canvas::SetLeft(rect, x);
            canvas.Children().Append(rect);
        }
    }

    void MainWindow::OnConnectionEstablished()
    {
        OutputDebugStringW(L"MainWindow: Connection established\n");
        ConnectionDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0, 200, 0)));
        ConnectionStatusText().Text(L"Connected");
    }

    void MainWindow::OnConnectionLost()
    {
        OutputDebugStringW(L"MainWindow: Connection lost\n");
        ConnectionDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 255, 0, 0)));
        ConnectionStatusText().Text(L"Disconnected");
    }

    void MainWindow::OnConnectionError(const hstring& error)
    {
        OutputDebugStringW((L"MainWindow: Connection error - " + error + L"\n").c_str());
        ConnectionDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 255, 0, 0)));
        ConnectionStatusText().Text(L"Error");
    }

    void MainWindow::SetupContainerCallbacks()
    {
        m_container->OnContainerStarted = [this]()
        {
            m_dispatcherQueue.TryEnqueue([this]()
            {
                OutputDebugStringW(L"MainWindow: Container started\n");
                ContainerDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 0, 200, 0)));
                ContainerStatusText().Text(L"Container: Running");
            });
        };

        m_container->OnContainerStopped = [this]()
        {
            m_dispatcherQueue.TryEnqueue([this]()
            {
                OutputDebugStringW(L"MainWindow: Container stopped\n");
                ContainerDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 128, 128, 128)));
                ContainerStatusText().Text(L"Container: Exited");
            });
        };

        m_container->OnContainerError = [this](const hstring& error)
        {
            m_dispatcherQueue.TryEnqueue([this, error]()
            {
                OutputDebugStringW((L"MainWindow: Container error - " + error + L"\n").c_str());
                ContainerDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 255, 0, 0)));
                ContainerStatusText().Text(L"Container: " + error);
            });
        };

        m_container->OnContainerOutput = [this](const hstring& output)
        {
            m_dispatcherQueue.TryEnqueue([this, output]()
            {
                OutputDebugStringW((L"MainWindow: Container output - " + output + L"\n").c_str());
                ContainerStatusText().Text(L"Container: " + output);
            });
        };

        // Set initial "starting" state
        ContainerDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 255, 165, 0)));
        ContainerStatusText().Text(L"Container: Starting...");
    }
}
