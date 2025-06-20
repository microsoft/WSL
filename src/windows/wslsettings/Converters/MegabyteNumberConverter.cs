// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Converters
{
    public sealed class MegabyteNumberConverter : Microsoft.UI.Xaml.Data.IValueConverter
    {
        public object? Convert(object value, Type targetType, object parameter, string language)
        {
            if (value != null && UInt64.TryParse(value as string, out UInt64 parseResult))
            {
                return (parseResult / Constants.MB).ToString();
            }

            return null;
        }

        public object? ConvertBack(object value, Type targetType, object parameter, string language)
        {

            if (value != null && UInt64.TryParse(value as string, out UInt64 parseResult))
            {
                return (parseResult * Constants.MB).ToString();
            }

            return null;
        }
    }
}
