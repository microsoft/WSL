// Copyright (C) Microsoft Corporation. All rights reserved.

using System.Text.RegularExpressions;

namespace WslSettings.Helpers;

public class Constants
{
    public static uint MB = 1024 * 1024;
    public static Regex WholeNumberRegex = new Regex("^[0-9]+$");
    public static Regex IntegerRegex = new Regex("^((-[1-9][0-9]*)|([0-9]+))$");
    public static Regex CommaSeparatedWholeNumbersOrEmptyRegex = new Regex(@"^(([1-9][0-9]{0,3}|[1-5][0-9]{4}|6[0-4][0-9]{3}|65[0-4][0-9]{2}|655[0-2][0-9]|6553[0-5])[,])*([1-9][0-9]{0,3}|[1-5][0-9]{4}|6[0-4][0-9]{3}|65[0-4][0-9]{2}|655[0-2][0-9]|6553[0-5])?$");
}