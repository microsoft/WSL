/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLITableOutputUnitTests.cpp

Abstract:

    Unit tests for the TableOutput class.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "TableOutput.h"

using namespace wsl::windows::wslc;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCTableOutputUnitTests {

class WSLCTableOutputUnitTests
{
    WSLC_TEST_CLASS(WSLCTableOutputUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // Test: header line is emitted as the first row, even with no data rows.
    TEST_METHOD(TableOutput_AlwaysShowHeader_EmitsHeaderWhenEmpty)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetAlwaysShowHeader(true);

        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines.size());
        // Header line must contain both column names
        VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[0].find(L"STATUS") != std::wstring::npos);
    }

    // Test: no output at all when empty and AlwaysShowHeader is false.
    TEST_METHOD(TableOutput_NoHeader_EmitsNothingWhenEmpty)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetAlwaysShowHeader(false);

        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines.size());
    }

    // Test: one data row produces header + one data line.
    TEST_METHOD(TableOutput_SingleRow_EmitsHeaderPlusOneDataLine)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

        cap.table.OutputLine({L"my-container", L"running"});
        cap.table.Complete();

        // Expect: header row + 1 data row = 2 lines total
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines.size());
        VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[1].find(L"my-container") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[1].find(L"running") != std::wstring::npos);
    }

    // Test: multiple data rows all appear after the header.
    TEST_METHOD(TableOutput_MultipleRows_AllRowsEmittedAfterHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

        cap.table.OutputLine({L"container-a", L"running"});
        cap.table.OutputLine({L"container-b", L"stopped"});
        cap.table.OutputLine({L"container-c", L"paused"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(4), cap.lines.size()); // header + 3 rows

        VERIFY_IS_TRUE(cap.lines[1].find(L"container-a") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[2].find(L"container-b") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[3].find(L"container-c") != std::wstring::npos);
    }

    // Test: columns are separated by the correct number of spaces.
    TEST_METHOD(TableOutput_ColumnPadding_DefaultPaddingApplied)
    {
        // Use a custom padding of 3 (the default) and verify the data row
        // contains at least 3 spaces between the first column value and the
        // start of the second column value.
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, /*sizingBuffer=*/50, /*columnPadding=*/3);

        cap.table.OutputLine({L"abc", L"ok"});
        cap.table.Complete();

        // Data row: "abc" padded to header width ("NAME"=4) + 3 spaces, then "ok"
        // Expected: "abc   ok" with appropriate spacing
        const std::wstring& dataLine = cap.lines[1];
        VERIFY_IS_TRUE(dataLine.find(L"abc") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"ok") != std::wstring::npos);

        // There must be at least 3 spaces between the two values
        auto columnPadding = 3;
        auto namePos = dataLine.find(L"abc");
        auto statusPos = dataLine.find(L"ok");
        VERIFY_IS_TRUE(statusPos >= namePos + wcslen(L"abc") + columnPadding);
    }

    // Test: custom column padding is respected.
    TEST_METHOD(TableOutput_ColumnPadding_CustomPaddingApplied)
    {
        constexpr size_t customPadding = 5;
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"A", L"B"}, /*sizingBuffer=*/50, customPadding);

        cap.table.OutputLine({L"x", L"y"});
        cap.table.Complete();

        // "A" header is 1 char wide, "x" value is 1 char wide.
        // With 5-space padding, "y" must start at position >= 1 + 5 = 6.
        const std::wstring& dataLine = cap.lines[1];
        auto posX = dataLine.find(L'x');
        auto posY = dataLine.find(L'y');
        VERIFY_IS_TRUE(posX != std::wstring::npos);
        VERIFY_IS_TRUE(posY != std::wstring::npos);
        VERIFY_IS_TRUE(posY >= posX + 1 + customPadding);
    }

    // Test: column width expands to fit the widest data value.
    TEST_METHOD(TableOutput_ColumnWidth_ExpandsToFitData)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"ID", L"NAME"});

        cap.table.OutputLine({L"1", L"short"});
        cap.table.OutputLine({L"2", L"a-very-long-container-name"});
        cap.table.Complete();

        // The second column must accommodate the widest value in every row.
        for (size_t i = 1; i < cap.lines.size(); ++i)
        {
            // The long value must not have been truncated.
            if (cap.lines[i].find(L"a-very-long-container-name") != std::wstring::npos)
            {
                LogComment(L"Long value found intact in row " + std::to_wstring(i));
            }
        }
        VERIFY_IS_TRUE(cap.lines[2].find(L"a-very-long-container-name") != std::wstring::npos);
    }

    // Test: column width is at least as wide as the header.
    TEST_METHOD(TableOutput_ColumnWidth_AtLeastHeaderWidth)
    {
        // Header "CONTAINER_NAME" is 14 chars; data value is only 3 chars.
        // The data line must still be padded to the header width.
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"CONTAINER_NAME", L"ST"});

        cap.table.OutputLine({L"abc", L"ok"});
        cap.table.Complete();

        // Header line: "CONTAINER_NAME" starts at position 0.
        // Data line: "abc" starts at position 0, "ok" must not start before
        // position 14 + padding.
        const std::wstring& dataLine = cap.lines[1];
        auto posOk = dataLine.find(L"ok");
        VERIFY_IS_TRUE(posOk != std::wstring::npos);
        // "CONTAINER_NAME" = 14 chars, padding = 3 -> "ok" must be at >= 17
        VERIFY_IS_TRUE(posOk >= static_cast<size_t>(14 + TableOutput<2>::DefaultColumnPadding));
    }

    // Test: values exceeding MaxWidth are truncated and an ellipsis appended.
    TEST_METHOD(TableOutput_MaxWidth_LongValueIsTruncatedWithEllipsis)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 8; // limit first column to 8 chars
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, std::move(configs));

        cap.table.OutputLine({L"a-very-long-name", L"running"});
        cap.table.Complete();

        const std::wstring& dataLine = cap.lines[1];
        // Ellipsis character (U+2026) must be present
        VERIFY_IS_TRUE(dataLine.find(L"\x2026") != std::wstring::npos);
        // Full original value must NOT be present
        VERIFY_IS_TRUE(dataLine.find(L"a-very-long-name") == std::wstring::npos);
    }

    // Test: values within MaxWidth are not truncated.
    TEST_METHOD(TableOutput_MaxWidth_ShortValueNotTruncated)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 20;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, std::move(configs));

        cap.table.OutputLine({L"short", L"running"});
        cap.table.Complete();

        const std::wstring& dataLine = cap.lines[1];
        VERIFY_IS_TRUE(dataLine.find(L"short") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"\x2026") == std::wstring::npos);
    }

    // Test: columns shrink when total width exceeds console width.
    TEST_METHOD(TableOutput_ConsoleWidthLimit_PreferredShrinkColumnIsShrunk)
    {
        // Two columns, first marked preferredShrink=false, second preferredShrink=true.
        // With a very narrow console the second column should absorb the cut.
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = ColumnWidthConfig::NoLimit;
        configs[0].PreferredShrink = false;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;
        configs[1].PreferredShrink = true;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"ID", L"DESCRIPTION"}, std::move(configs));
        // Override with a very narrow console: only 20 chars wide.
        cap.table.SetConsoleWidthOverride(20);
        cap.table.SetColumnWidthLimiting(true);

        cap.table.OutputLine({L"abc123", L"this-is-a-long-description-value"});
        cap.table.Complete();

        // The output must fit within 20 chars.
        for (const auto& line : cap.lines)
        {
            VERIFY_IS_TRUE(line.size() <= static_cast<size_t>(20));
        }
    }

    // Test: IsEmpty returns true before any rows are added, and false after a row is added.
    TEST_METHOD(TableOutput_IsEmpty)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        VERIFY_IS_TRUE(cap.table.IsEmpty());

        cap.table.OutputLine({L"foo", L"bar"});
        VERIFY_IS_FALSE(cap.table.IsEmpty());
    }

    // Test: column-definition constructor wires up names and configs correctly.
    TEST_METHOD(TableOutput_ColumnDefinition_NameAndConfigUsed)
    {
        TableOutput<2>::column_def_t defs{{
            ColumnDefinition{L"MYID", {ColumnWidthConfig::NoLimit, 6, false}},
            ColumnDefinition{L"MYNAME", {ColumnWidthConfig::NoLimit, ColumnWidthConfig::NoLimit, true}},
        }};

        TableOutputCapture<2> cap(std::move(defs));

        cap.table.OutputLine({L"id-value", L"name-value"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines.size());
        VERIFY_IS_TRUE(cap.lines[0].find(L"MYID") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[0].find(L"MYNAME") != std::wstring::npos);

        // "id-value" is 8 chars but MaxWidth=6 -> must be truncated
        VERIFY_IS_TRUE(cap.lines[1].find(L"\x2026") != std::wstring::npos);
    }

    // Test: SetShowHeader(false) suppresses header when there are data rows.
    TEST_METHOD(TableOutput_ShowHeader_False_SuppressesHeaderWithDataRows)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"my-container", L"running"});
        cap.table.Complete();

        // Only the data row should be emitted.
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines.size());
        VERIFY_IS_TRUE(cap.lines[0].find(L"my-container") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") == std::wstring::npos);
    }

    // Test: SetShowHeader(false) with AlwaysShowHeader(true) still suppresses header when empty.
    TEST_METHOD(TableOutput_ShowHeader_False_SuppressesHeaderEvenWhenAlwaysShowHeaderTrue)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetAlwaysShowHeader(true);
        cap.table.SetShowHeader(false);

        cap.table.Complete();

        // SetShowHeader(false) takes precedence. Nothing should be emitted.
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines.size());
    }

    // Test: SetShowHeader(true) is the default. Header appears before data rows.
    TEST_METHOD(TableOutput_ShowHeader_True_IsDefaultAndEmitsHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        // No explicit call to SetShowHeader. Default must be true.

        cap.table.OutputLine({L"my-container", L"running"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines.size());
        VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[0].find(L"STATUS") != std::wstring::npos);
    }

    // Test: SetShowHeader(false) with multiple data rows emits only data rows.
    TEST_METHOD(TableOutput_ShowHeader_False_MultipleDataRowsNoHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"container-a", L"running"});
        cap.table.OutputLine({L"container-b", L"stopped"});
        cap.table.Complete();

        // Two data rows, zero header rows.
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines.size());
        VERIFY_IS_TRUE(cap.lines[0].find(L"container-a") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[1].find(L"container-b") != std::wstring::npos);
        // Neither line should contain the column header text.
        VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") == std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines[1].find(L"NAME") == std::wstring::npos);
    }

    // Test: SetShowHeader controls whether the header row is emitted.
    // Covers: default (true), suppression with data rows, suppression when empty
    // (even with AlwaysShowHeader), and multiple data rows with no header.
    TEST_METHOD(TableOutput_ShowHeader)
    {
        // Default is true. Header appears before data rows without an explicit call.
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

            cap.table.OutputLine({L"my-container", L"running"});
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines.size());
            VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines[0].find(L"STATUS") != std::wstring::npos);
        }

        // SetShowHeader(false) suppresses the header when data rows are present.
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
            cap.table.SetShowHeader(false);

            cap.table.OutputLine({L"my-container", L"running"});
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines.size());
            VERIFY_IS_TRUE(cap.lines[0].find(L"my-container") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") == std::wstring::npos);
        }

        // SetShowHeader(false) suppresses the header even when AlwaysShowHeader is true and the table is empty.
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
            cap.table.SetAlwaysShowHeader(true);
            cap.table.SetShowHeader(false);

            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines.size());
        }

        // SetShowHeader(false) with multiple data rows emits only data rows.
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
            cap.table.SetShowHeader(false);

            cap.table.OutputLine({L"container-a", L"running"});
            cap.table.OutputLine({L"container-b", L"stopped"});
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines.size());
            VERIFY_IS_TRUE(cap.lines[0].find(L"container-a") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines[1].find(L"container-b") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines[0].find(L"NAME") == std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines[1].find(L"NAME") == std::wstring::npos);
        }
    }
};

} // namespace WSLCTableOutputUnitTests