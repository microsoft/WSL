// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.UI.ViewManagement;

namespace WslSettings.Controls
{
    public sealed partial class OOBEContent : UserControl
    {
        public OOBEContent()
        {
            this.InitializeComponent();

            // Adjust hero image height based on text scaling for better accessibility
            var settings = new UISettings();
            double textScaleFactor = settings.TextScaleFactor;
            double baseImageHeight = 280.0;

            // Reduce image height when text scaling increases to preserve content space
            // Use inverse relationship: as text gets larger, image gets proportionally smaller
            HeroImageHeight = Math.Max(baseImageHeight / textScaleFactor, 200.0);
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
        public static readonly DependencyProperty HeroImageHeightProperty = DependencyProperty.Register("HeroImageHeight", typeof(double), typeof(OOBEContent), new PropertyMetadata(280.0));
    }
}