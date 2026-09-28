// Copyright (C) Microsoft Corporation. All rights reserved.

namespace WslSettings.Converters
{
    public sealed class MegabyteStringConverter : Microsoft.UI.Xaml.Data.IValueConverter
    {
        public object? Convert(object value, Type targetType, object parameter, string language)
        {
            if (value == null)
            {
                return null;
            }

            return string.Format("Settings_MegabyteStringFormat".GetLocalized(), (System.Convert.ToUInt64(value) / Constants.MB));
        }

        public object ConvertBack(object value, Type targetType, object parameter, string language)
        {
            throw new NotImplementedException();
        }
    }
}
