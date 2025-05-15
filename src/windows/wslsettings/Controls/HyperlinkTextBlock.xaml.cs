// Copyright (C) Microsoft Corporation. All rights reserved.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace WslSettings.Controls;

/// <summary>
/// Custom control for a text block that contains a hyperlink in the text.
/// </summary>
/// <remarks>
/// The <see cref="Text"/> property must contain a substring enclosed in square
/// brackets ('[' and ']'). When displaying the text block, the brackets are
/// removed and the text inside is made into a link that points to <see cref="NavigateUri"/>
/// </remarks>
public sealed partial class HyperlinkTextBlock : UserControl
{
    /// <summary>
    /// Gets or sets the text for the text block. This must contain
    /// a substring contained within square brackets ('[', ']')
    /// </summary>
    public string Text
    {
        get => (string)GetValue(TextProperty);
        set
        {
            SetValue(TextProperty, value);

            var openingBracketIndex = value.IndexOf('[');
            var closingBracketIndex = value.IndexOf(']');

            if (openingBracketIndex == -1 || closingBracketIndex == -1
                || openingBracketIndex > closingBracketIndex)
            {
                // If there is not string contained between brackets, show the text as is
                TextBeforeHyperlink = value;
                HyperLinkText = string.Empty;
                TextAfterHyperlink = string.Empty;
            }
            else
            {
                TextBeforeHyperlink = value.Substring(0, openingBracketIndex);
                HyperLinkText = value.Substring(openingBracketIndex + 1, closingBracketIndex - openingBracketIndex - 1);
                TextAfterHyperlink = value.Substring(closingBracketIndex + 1);
            }
        }
    }

    public string NavigateUri
    {
        get => (string)GetValue(NavigateUriProperty);
        set => SetValue(NavigateUriProperty, value);
    }

    internal string TextBeforeHyperlink { get; private set; } = string.Empty;

    internal string HyperLinkText { get; private set; } = string.Empty;

    internal string TextAfterHyperlink { get; private set; } = string.Empty;

    public HyperlinkTextBlock()
    {
        InitializeComponent();
    }

    public static readonly DependencyProperty TextProperty = DependencyProperty.Register(nameof(Text), typeof(string), typeof(HyperlinkTextBlock), new PropertyMetadata(string.Empty));
    public static readonly DependencyProperty NavigateUriProperty = DependencyProperty.Register(nameof(NavigateUri), typeof(string), typeof(HyperlinkTextBlock), new PropertyMetadata(string.Empty));
}