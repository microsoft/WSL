// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Converters
{
    public sealed class MillisecondsStringConverter : Microsoft.UI.Xaml.Data.IValueConverter
    {
        public object? Convert(object value, Type targetType, object parameter, string language)
        {
            if (value == null || (value is ulong && (ulong)value == 0))
            {
                return null;
            }

            return string.Format("Settings_MillisecondsStringFormat".GetLocalized(), value);
        }

        public object ConvertBack(object value, Type targetType, object parameter, string language)
        {
            throw new NotImplementedException();
        }
    }
}
