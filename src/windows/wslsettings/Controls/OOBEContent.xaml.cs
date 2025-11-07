// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.UI.ViewManagement;

namespace WslSettings.Controls
{
    public sealed partial class OOBEContent : UserControl
    {
        // Constants for hero image height calculations
        private const double BaseImageHeight = 280.0;
        private const double MinimumImageHeight = 200.0;

        private static readonly UISettings Settings = new UISettings();

        public OOBEContent()
        {
            this.InitializeComponent();

            // Set initial hero image height based on current text scaling
            UpdateHeroImageHeight();

            // Subscribe to text scale factor changes for dynamic updates
            Settings.TextScaleFactorChanged += OnTextScaleFactorChanged;

            // Ensure event cleanup when control is unloaded
            this.Unloaded += (s, e) => Settings.TextScaleFactorChanged -= OnTextScaleFactorChanged;
        }

        private void UpdateHeroImageHeight()
        {
            double textScaleFactor = Settings.TextScaleFactor;

            // Reduce image height when text scaling increases to preserve content space
            // Use inverse relationship: as text gets larger, image gets proportionally smaller
            HeroImageHeight = Math.Max(BaseImageHeight / textScaleFactor, MinimumImageHeight);
        }

        private void OnTextScaleFactorChanged(UISettings sender, object args)
        {
            // Update hero image height when text scaling changes at runtime
            this.DispatcherQueue.TryEnqueue(() => UpdateHeroImageHeight());
        }

        public string Title
        {
            get { return (string)GetValue(TitleProperty); }
            set { SetValue(TitleProperty, value); }
        }

        public string Description
        {
            get => (string)GetValue(DescriptionProperty);
            set => SetValue(DescriptionProperty, value);
        }

        public string HeroImage
        {
            get => (string)GetValue(HeroImageProperty);
            set => SetValue(HeroImageProperty, value);
        }

        public double HeroImageHeight
        {
            get { return (double)GetValue(HeroImageHeightProperty); }
            set { SetValue(HeroImageHeightProperty, value); }
        }

        public object PageContent
        {
            get { return (object)GetValue(PageContentProperty); }
            set { SetValue(PageContentProperty, value); }
        }

        public static readonly DependencyProperty TitleProperty = DependencyProperty.Register("Title", typeof(string), typeof(OOBEContent), new PropertyMetadata(default(string)));
        public static readonly DependencyProperty DescriptionProperty = DependencyProperty.Register("Description", typeof(string), typeof(OOBEContent), new PropertyMetadata(default(string)));
        public static readonly DependencyProperty HeroImageProperty = DependencyProperty.Register("HeroImage", typeof(string), typeof(OOBEContent), new PropertyMetadata(default(string)));
        public static readonly DependencyProperty PageContentProperty = DependencyProperty.Register("PageContent", typeof(object), typeof(OOBEContent), new PropertyMetadata(new Grid()));
        public static readonly DependencyProperty HeroImageHeightProperty = DependencyProperty.Register("HeroImageHeight", typeof(double), typeof(OOBEContent), new PropertyMetadata(BaseImageHeight));
    }
}