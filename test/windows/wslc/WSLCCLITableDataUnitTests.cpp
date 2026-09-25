/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLITableDataUnitTests.cpp

Abstract:

    Unit tests for the TableData model and renderer.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "TableRenderer.h"
#include "VTSupport.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::cli;
using namespace wsl::windows::common::vt;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLITableDataUnitTests {

class WSLCCLITableDataUnitTests
{
    WSLC_TEST_CLASS(WSLCCLITableDataUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(TableData_EmptyTable_EmitsHeader)
    {
        TableCapture cap({L"NAME", L"STATUS"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
    }

    TEST_METHOD(TableData_NoHeader_EmitsNothingWhenEmpty)
    {
        TableCapture cap({L"NAME", L"STATUS"});
        cap.table.ShowHeader = false;
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines().size());
    }

    TEST_METHOD(TableData_SingleRow_EmitsHeaderPlusOneDataLine)
    {
        TableCapture cap({L"NAME", L"STATUS"});

        cap.table.AddRow({L"my-container", L"running"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"my-container") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"running") != std::wstring::npos);
    }

    TEST_METHOD(TableData_MultipleRows_AllRowsEmittedAfterHeader)
    {
        TableCapture cap({L"NAME", L"STATUS"});

        cap.table.AddRow({L"container-a", L"running"});
        cap.table.AddRow({L"container-b", L"stopped"});
        cap.table.AddRow({L"container-c", L"paused"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(4), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[1].find(L"container-a") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[2].find(L"container-b") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[3].find(L"container-c") != std::wstring::npos);
    }

    TEST_METHOD(TableData_ColumnPadding_DefaultPaddingApplied)
    {
        TableCapture cap({L"NAME", L"STATUS"});

        cap.table.AddRow({L"abc", L"ok"});
        cap.Render();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
        VERIFY_IS_TRUE(dataLine.find(L"abc") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"ok") != std::wstring::npos);

        auto columnPadding = 3;
        auto namePos = dataLine.find(L"abc");
        auto statusPos = dataLine.find(L"ok");
        VERIFY_IS_TRUE(statusPos >= namePos + wcslen(L"abc") + columnPadding);
    }

    TEST_METHOD(TableData_ColumnPadding_CustomPaddingApplied)
    {
        constexpr size_t customPadding = 5;
        TableCapture cap({L"A", L"B"});
        cap.table.ColumnPadding = customPadding;

        cap.table.AddRow({L"x", L"y"});
        cap.Render();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
        auto posX = dataLine.find(L'x');
        auto posY = dataLine.find(L'y');
        VERIFY_IS_TRUE(posX != std::wstring::npos);
        VERIFY_IS_TRUE(posY != std::wstring::npos);
        VERIFY_IS_TRUE(posY >= posX + 1 + customPadding);
    }

    TEST_METHOD(TableData_ColumnWidth_ExpandsToFitData)
    {
        TableCapture cap({L"ID", L"NAME"});

        cap.table.AddRow({L"1", L"short"});
        cap.table.AddRow({L"2", L"a-very-long-container-name"});
        cap.Render();

        VERIFY_IS_TRUE(cap.lines()[2].find(L"a-very-long-container-name") != std::wstring::npos);
    }

    TEST_METHOD(TableData_ColumnWidth_AtLeastHeaderWidth)
    {
        TableCapture cap({L"CONTAINER_NAME", L"ST"});

        cap.table.AddRow({L"abc", L"ok"});
        cap.Render();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
        auto posOk = dataLine.find(L"ok");
        VERIFY_IS_TRUE(posOk != std::wstring::npos);
        VERIFY_IS_TRUE(posOk >= static_cast<size_t>(14 + c_defaultColumnPadding));
    }

    TEST_METHOD(TableData_MinCellWidth_DefaultIsAppliedToNonFinalColumns)
    {
        const TableData defaults{L"A", L"B"};
        VERIFY_ARE_EQUAL(c_defaultMinCellWidth, defaults.MinCellWidth);

        TableCapture cap({L"A", L"B", L"C"});
        cap.table.MinCellWidth = c_defaultMinCellWidth;

        cap.table.AddRow({L"x", L"y", L"z"});
        cap.Render();

        // Columns narrower than the minimum are widened so each cell, padding included, occupies
        // the minimum width.
        const auto dataLine = cap.lines()[1];
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), dataLine.find(L'x'));
        VERIFY_ARE_EQUAL(c_defaultMinCellWidth, dataLine.find(L'y'));
        VERIFY_ARE_EQUAL(c_defaultMinCellWidth * 2, dataLine.find(L'z'));
    }

    TEST_METHOD(TableData_MinCellWidth_FinalColumnIsExempt)
    {
        TableCapture cap({L"A", L"B"});
        cap.table.MinCellWidth = c_defaultMinCellWidth;

        cap.table.AddRow({L"x", L"y"});
        cap.Render();

        // The final column renders at its natural width, so no line carries trailing whitespace.
        for (const auto& line : cap.lines())
        {
            VERIFY_ARE_EQUAL(c_defaultMinCellWidth + 1, line.size());
        }
    }

    TEST_METHOD(TableData_MinCellWidth_WiderColumnKeepsItsWidth)
    {
        TableCapture cap({L"CONTAINER_NAME", L"STATUS"});
        cap.table.MinCellWidth = c_defaultMinCellWidth;

        cap.table.AddRow({L"abc", L"ok"});
        cap.Render();

        VERIFY_ARE_EQUAL(wcslen(L"CONTAINER_NAME") + c_defaultColumnPadding, cap.lines()[1].find(L"ok"));
    }

    TEST_METHOD(TableData_MaxWidth_LongValueIsTruncatedWithEllipsis)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 8;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;

        TableCapture cap(std::vector<ColumnDefinition>{{L"NAME", configs[0]}, {L"STATUS", configs[1]}});

        cap.table.AddRow({L"a-very-long-name", L"running"});
        cap.Render();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
        VERIFY_IS_TRUE(dataLine.find(L"\x2026") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"a-very-long-name") == std::wstring::npos);
    }

    TEST_METHOD(TableData_MaxWidth_ShortValueNotTruncated)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 20;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;

        TableCapture cap(std::vector<ColumnDefinition>{{L"NAME", configs[0]}, {L"STATUS", configs[1]}});

        cap.table.AddRow({L"short", L"running"});
        cap.Render();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
        VERIFY_IS_TRUE(dataLine.find(L"short") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"\x2026") == std::wstring::npos);
    }

    TEST_METHOD(TableData_ConsoleWidthLimit_PreferredShrinkColumnIsShrunk)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = ColumnWidthConfig::NoLimit;
        configs[0].Overflow = ColumnOverflow::Shrink;
        configs[0].PreferredShrink = false;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;
        configs[1].Overflow = ColumnOverflow::Shrink;
        configs[1].PreferredShrink = true;

        TableCapture cap(std::vector<ColumnDefinition>{{L"ID", configs[0]}, {L"DESCRIPTION", configs[1]}});
        cap.table.ConsoleWidthOverride = 20;

        cap.table.AddRow({L"abc123", L"this-is-a-long-description-value"});
        cap.Render();

        auto lines = cap.lines();
        for (const auto& line : lines)
        {
            VERIFY_IS_TRUE(line.size() <= static_cast<size_t>(20));
        }
    }

    TEST_METHOD(TableData_IsEmpty)
    {
        TableCapture cap({L"NAME", L"STATUS"});
        VERIFY_IS_TRUE(cap.table.IsEmpty());

        cap.table.AddRow({L"foo", L"bar"});
        VERIFY_IS_FALSE(cap.table.IsEmpty());
    }

    TEST_METHOD(TableData_ColumnDefinition_NameAndConfigUsed)
    {
        std::vector<ColumnDefinition> defs{
            ColumnDefinition{
                L"MYID",
                {
                    .MinWidth = ColumnWidthConfig::NoLimit,
                    .MaxWidth = 6,
                    .Overflow = ColumnOverflow::Shrink,
                    .PreferredShrink = false,
                }},
            ColumnDefinition{L"MYNAME", {.MinWidth = ColumnWidthConfig::NoLimit, .MaxWidth = ColumnWidthConfig::NoLimit}},
        };

        TableCapture cap(std::move(defs));

        cap.table.AddRow({L"id-value", L"name-value"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"MYID") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"MYNAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"\x2026") != std::wstring::npos);
    }

    TEST_METHOD(TableData_ShowHeader_False_SuppressesHeaderWithDataRows)
    {
        TableCapture cap({L"NAME", L"STATUS"});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"my-container", L"running"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"my-container") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
    }

    TEST_METHOD(TableData_ShowHeader_False_SuppressesHeaderWhenEmpty)
    {
        TableCapture cap({L"NAME", L"STATUS"});
        cap.table.ShowHeader = false;
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines().size());
    }

    TEST_METHOD(TableData_ShowHeader_True_IsDefaultAndEmitsHeader)
    {
        TableCapture cap({L"NAME", L"STATUS"});

        cap.table.AddRow({L"my-container", L"running"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
    }

    TEST_METHOD(TableData_ShowHeader_False_MultipleDataRowsNoHeader)
    {
        TableCapture cap({L"NAME", L"STATUS"});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"container-a", L"running"});
        cap.table.AddRow({L"container-b", L"stopped"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"container-a") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"container-b") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"NAME") == std::wstring::npos);
    }

    TEST_METHOD(TableData_ShowHeader)
    {
        {
            TableCapture cap({L"NAME", L"STATUS"});

            cap.table.AddRow({L"my-container", L"running"});
            cap.Render();

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
            VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
        }
        {
            TableCapture cap({L"NAME", L"STATUS"});
            cap.table.ShowHeader = false;

            cap.table.AddRow({L"my-container", L"running"});
            cap.Render();

            VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
            VERIFY_IS_TRUE(cap.lines()[0].find(L"my-container") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
        }
        {
            TableCapture cap({L"NAME", L"STATUS"});
            cap.table.ShowHeader = false;
            cap.Render();

            VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines().size());
        }
        {
            TableCapture cap({L"NAME", L"STATUS"});
            cap.table.ShowHeader = false;

            cap.table.AddRow({L"container-a", L"running"});
            cap.table.AddRow({L"container-b", L"stopped"});
            cap.Render();

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
            VERIFY_IS_TRUE(cap.lines()[0].find(L"container-a") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[1].find(L"container-b") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[1].find(L"NAME") == std::wstring::npos);
        }
    }

    TEST_METHOD(TableData_RowIndent_PrependedToEveryRow)
    {
        TableCapture cap({L"NAME", L"STATUS"});
        cap.table.RowIndent = 2;

        cap.table.AddRow({L"abc", L"ok"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"  NAME   STATUS"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"  abc    ok"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_ShortValueProducesOneRow)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", L"short desc"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   short desc"}, cap.lines()[0]);
    }

    TEST_METHOD(TableData_WordWrap_WrapsAtWordBoundary)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", L"hello world"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_ContinuationRowHasBlankLeadingColumns)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", L"hello world"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"      world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_MultipleWrapsProduceMultipleRows)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", L"one two three four"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   one two"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      three four"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_HardBreakWhenNoSpaceFound)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 6;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", L"abcdefghij"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   abcdef"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      ghij"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_NonWrappingColumnStillTruncates)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 5;
        // ColumnOverflow::Truncate (default) — truncates
        configs[1].MaxWidth = 20;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"toolongname", L"short desc"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"tool\u2026   short desc"}, cap.lines()[0]);
    }

    TEST_METHOD(TableData_WordWrap_RowIndentAppliedToAllPhysicalRows)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;
        cap.table.RowIndent = 2;

        cap.table.AddRow({L"opt", L"hello world"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"  opt   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"        world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_DisabledByDefault_LongTextTruncated)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 8;
        // ColumnOverflow::Truncate (default) — truncates

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", L"a very long description"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   a very \u2026"}, cap.lines()[0]);
    }

    TEST_METHOD(TableData_WordWrap_MultipleLogicalRowsEachWrapIndependently)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt-a", L"hello world"});
        cap.table.AddRow({L"opt-b", L"short"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt-a   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"        world"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"opt-b   short"}, cap.lines()[2]);
    }

    TEST_METHOD(TableData_WordWrap_TwoWrappingColumnsColBLonger)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 5;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"ab cd ef", L"one two three"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"ab cd   one"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"ef      two"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"        three"}, cap.lines()[2]);
    }

    TEST_METHOD(TableData_WordWrap_TwoWrappingColumnsColALonger)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 5;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"one two three", L"ab cd ef"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"one     ab cd"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"two     ef"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"three   "}, cap.lines()[2]);
    }

    TEST_METHOD(TableData_WordWrap_TwoWrappingColumnsWithEqualLengths)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 4;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 4;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"aa bb", L"xx yy"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"aa     xx"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"bb     yy"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_NonWrappingColumnBetweenTwoWrappingColumns)
    {
        ColumnWidthConfig configs[3]{};
        configs[0].MaxWidth = 4;
        // configs[0].Overflow = ColumnOverflow::Truncate (default) — truncates
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;
        configs[2].MaxWidth = 5;
        configs[2].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}, {L"", configs[2]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"tag", L"aa bb", L"xx yy zz"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"tag   aa bb   xx yy"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"              zz"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_WordWrap_FirstColumnLongerThanSecond)
    {
        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 5;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"one two three", L"ab cd ef"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"one     ab cd"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"two     ef"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"three   "}, cap.lines()[2]);
    }

    TEST_METHOD(TableData_ColumnConfig_WordWrapAfterConstruction)
    {
        TableCapture cap({L"", L""});
        cap.table.ShowHeader = false;
        cap.table.Columns[1].Config = ColumnWidthConfig{
            .MaxWidth = 8,
            .Overflow = ColumnOverflow::Wrap,
        };

        cap.table.AddRow({L"opt", L"hello world"});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      world"}, cap.lines()[1]);
    }

    TEST_METHOD(Cell_VisibleWidth_PlainText)
    {
        Cell cell{L"hello"};
        VERIFY_ARE_EQUAL(static_cast<size_t>(5), cell.VisibleWidth());
    }

    TEST_METHOD(Cell_VisibleWidth_StyledText)
    {
        Cell cell{L"hello", Format::Fg::BrightRed};
        VERIFY_ARE_EQUAL(static_cast<size_t>(5), cell.VisibleWidth());
    }

    TEST_METHOD(Cell_Render_ColorEnabled)
    {
        Cell cell = Cell(L"hello", Format::Fg::BrightRed);
        const auto result = cell.Render(true, true);
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\x1b'));
    }

    TEST_METHOD(Cell_Render_ColorDisabled)
    {
        Cell cell = Cell(L"hello", Format::Fg::BrightRed);
        const auto result = cell.Render(false, false);
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, result);
    }

    TEST_METHOD(Cell_Render_VTEnabled_ColorDisabled_StripsColorSequences)
    {
        Cell cell = Cell(L"hello", Format::Fg::BrightRed);
        // VT on but color off: color sequences should be stripped
        const auto result = cell.Render(true, false);
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, result);
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L'\x1b'));
    }

    TEST_METHOD(Cell_RenderTruncated_TruncatesVisibleText)
    {
        Cell cell = Cell(L"hello world", Format::Fg::BrightRed);
        const auto result = cell.RenderTruncated(5, true, true);
        // Should contain truncated visible text + ellipsis + sequences
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L"world"));
    }

    TEST_METHOD(TableData_Cell_ColumnWidthBasedOnVisibleChars)
    {
        using namespace wsl::windows::common::vt;

        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}}, true);
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", Cell(L"hello", Format::Fg::BrightRed)});
        cap.table.AddRow({L"end", Cell{L"world"}});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        // First cell is emphasized (has ESC), second is plain
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring{L"end   world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableData_Cell_WrapsWithSequencesOnEachChunk)
    {
        using namespace wsl::windows::common::vt;

        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}}, true);
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", Cell(L"hello world", Format::Fg::BrightRed)});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        // Both lines should have emphasis sequences
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[1].find(L'\x1b'));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[1].find(L"world"));
    }

    TEST_METHOD(Cell_DefaultCtor_EmptyCell)
    {
        Cell cell;
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cell.VisibleWidth());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cell.Render(true, true));
        VERIFY_ARE_EQUAL(std::wstring{L""}, cell.Render(false, false));
    }

    TEST_METHOD(Cell_SingleSequenceCtor_BuildsPrefixWithReset)
    {
        Cell cell(L"bold", Format::Bright);
        VERIFY_ARE_EQUAL(static_cast<size_t>(4), cell.VisibleWidth());
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), static_cast<size_t>(cell.Prefix != nullptr) + static_cast<size_t>(cell.Suffix != nullptr));

        // Color enabled: sequences emitted
        const auto rendered = cell.Render(true, true);
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, rendered.find(L"bold"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, rendered.find(L'\x1b'));

        // Color disabled: plain text only
        VERIFY_ARE_EQUAL(std::wstring{L"bold"}, cell.Render(false, false));
    }

    TEST_METHOD(Cell_VisibleWidth_StyledMultipleCharacters)
    {
        Cell cell{L"abc", Format::Bright};
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cell.VisibleWidth());
    }

    TEST_METHOD(Cell_VisibleWidth_EmptyText)
    {
        Cell cell{L""};
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cell.VisibleWidth());
    }

    TEST_METHOD(Cell_VisibleWidth_EmptyStyledText)
    {
        Cell cell{L"", Format::Bright};
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cell.VisibleWidth());
    }

    TEST_METHOD(Cell_Render_PlainTextNoSequences)
    {
        Cell cell{L"plain text"};
        VERIFY_ARE_EQUAL(std::wstring{L"plain text"}, cell.Render(true, true));
        VERIFY_ARE_EQUAL(std::wstring{L"plain text"}, cell.Render(false, false));
    }

    TEST_METHOD(Cell_RenderTruncated_TextFitsNoTruncation)
    {
        Cell cell(L"hello", Format::Fg::BrightRed);
        const auto result = cell.RenderTruncated(10, true, true);
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"hello"));
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L'\u2026')); // no ellipsis
    }

    TEST_METHOD(Cell_RenderTruncated_ColorDisabled_StillTruncates)
    {
        Cell cell(L"hello world", Format::Fg::BrightRed);
        const auto result = cell.RenderTruncated(5, false, false);
        // Sequences stripped, but truncation still occurs
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L"world"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\u2026'));
    }

    TEST_METHOD(Cell_RenderTruncated_PlainText)
    {
        Cell cell{L"abcdefghij"};
        const auto result = cell.RenderTruncated(5, false, false);
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L"fghij"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\u2026'));
    }

    TEST_METHOD(TableData_Cell_ColorDisabled_SequencesStripped)
    {
        using namespace wsl::windows::common::vt;

        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;

        // vtEnabled=false: sequences should be stripped from output
        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}}, false);
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", Cell(L"hello", Format::Fg::BrightRed)});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        // No ESC bytes in output
        VERIFY_ARE_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        // Visible text still present with correct padding
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
    }

    TEST_METHOD(TableData_Cell_WrapColorDisabled_SequencesStripped)
    {
        using namespace wsl::windows::common::vt;

        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}}, false);
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt", Cell(L"hello world", Format::Fg::BrightRed)});
        cap.Render();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        // No ESC in either line
        VERIFY_ARE_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring::npos, cap.lines()[1].find(L'\x1b'));
        // Text still wraps correctly
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[1].find(L"world"));
    }

    TEST_METHOD(TableData_Cell_WrapMultipleLines_SequenceReapplied)
    {
        using namespace wsl::windows::common::vt;

        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}}, true);
        cap.table.ShowHeader = false;

        // Text that wraps into 3 lines: "aa bb cc" at width 5 -> "aa bb" / "cc"
        // Actually at width 5: "aa" "bb" "cc" (word-break at spaces)
        cap.table.AddRow({L"cmd", Cell(L"aa bb cc dd", Format::Fg::BrightCyan)});
        cap.Render();

        // Should produce multiple wrap lines, each with sequences
        auto lines = cap.lines();
        VERIFY_IS_GREATER_THAN(lines.size(), static_cast<size_t>(1));
        for (const auto& line : lines)
        {
            // Every line that has content should have the escape sequence reapplied
            if (!line.empty())
            {
                VERIFY_ARE_NOT_EQUAL(std::wstring::npos, line.find(L'\x1b'));
            }
        }
    }

    TEST_METHOD(TableData_Cell_HyperlinkTruncated_BothSequencesEmitted)
    {
        using namespace wsl::windows::common::vt;

        // Simulate a hyperlink cell: OSC 8 open + visible text + OSC 8 close.
        // The open/close are ConstructedSequences since they contain a URL.
        const ConstructedSequence hyperlinkOpen{L"\x1b]8;;https://example.com\x1b\\"};
        const ConstructedSequence hyperlinkClose{L"\x1b]8;;\x1b\\"};

        ColumnWidthConfig configs[2]{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8; // Force truncation of "click here now" (14 chars)

        TableCapture cap(std::vector<ColumnDefinition>{{L"", configs[0]}, {L"", configs[1]}}, true);
        cap.table.ShowHeader = false;

        // The cell wraps visible text with hyperlink open/close sequences.
        cap.table.AddRow({L"link", Cell(L"click here now", hyperlinkOpen, hyperlinkClose)});
        cap.Render();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), lines.size());
        const auto& line = lines[0];

        // The hyperlink open sequence must be present (starts the clickable region)
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, line.find(L"\x1b]8;;https://example.com\x1b\\"));
        // The hyperlink close sequence must also be present (terminates the clickable region)
        // Find the close AFTER the open
        auto openEnd = line.find(L"\x1b]8;;https://example.com\x1b\\");
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, openEnd);
        auto closePos = line.find(L"\x1b]8;;\x1b\\", openEnd + 1);
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, closePos);
        // Truncation occurred — full text should NOT be present
        VERIFY_ARE_EQUAL(std::wstring::npos, line.find(L"click here now"));
        // Ellipsis should be present
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, line.find(L'\u2026'));
    }

    TEST_METHOD(TableData_AddLine_BlankLineEmittedBetweenRows)
    {
        TableCapture cap({L"", L""});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"row-a", L"val-a"});
        cap.table.AddLine();
        cap.table.AddRow({L"row-b", L"val-b"});
        cap.Render();

        auto lines = cap.lines();
        // 3 lines: data, blank, data
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), lines.size());
        VERIFY_IS_TRUE(lines[0].find(L"row-a") != std::wstring::npos);
        VERIFY_IS_TRUE(lines[1].empty());
        VERIFY_IS_TRUE(lines[2].find(L"row-b") != std::wstring::npos);
    }

    TEST_METHOD(TableData_AddLine_SectionHeaderEmittedBetweenRows)
    {
        TableCapture cap({L"", L""});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"opt-a", L"desc-a"});
        cap.table.AddLine(Cell{L"Global Options:"});
        cap.table.AddRow({L"opt-b", L"desc-b"});
        cap.Render();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), lines.size());
        VERIFY_IS_TRUE(lines[0].find(L"opt-a") != std::wstring::npos);
        VERIFY_ARE_EQUAL(std::wstring{L"Global Options:"}, lines[1]);
        VERIFY_IS_TRUE(lines[2].find(L"opt-b") != std::wstring::npos);
    }

    TEST_METHOD(TableData_AddLine_DoesNotAffectColumnWidths)
    {
        // The break text is longer than any data cell; column widths should
        // be driven only by data rows, not breaks.
        TableCapture cap({L"", L""});
        cap.table.ShowHeader = false;

        cap.table.AddRow({L"ab", L"cd"});
        cap.table.AddLine(Cell{L"This is a very long section header that should not widen columns"});
        cap.table.AddRow({L"ef", L"gh"});
        cap.Render();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), lines.size());
        // Both data rows should have the same width (driven by "ab"/"ef" column)
        VERIFY_ARE_EQUAL(lines[0].size(), lines[2].size());
    }

    TEST_METHOD(TableData_AddLine_SharedColumnWidthsAcrossSections)
    {
        // Data rows in different sections share column widths because they
        // are sized together within a single table instance.
        TableCapture cap({L"", L""});
        cap.table.ShowHeader = false;

        cap.table.AddLine(Cell{L"Section A:"});
        cap.table.AddRow({L"short", L"x"});
        cap.table.AddLine(Cell{L"Section B:"});
        cap.table.AddRow({L"much-longer-name", L"y"});
        cap.Render();

        auto lines = cap.lines();
        // 4 lines: header, data, header, data
        VERIFY_ARE_EQUAL(static_cast<size_t>(4), lines.size());

        // "short" row should be padded to match "much-longer-name" column width.
        // Both data rows have the same total width.
        VERIFY_ARE_EQUAL(lines[1].size(), lines[3].size());
    }

    TEST_METHOD(TableData_AddLine_CellRendersSequences)
    {
        using namespace wsl::windows::common::vt;
        TableCapture cap(std::vector<ColumnDefinition>{{L"", {}}, {L"", {}}}, true);
        cap.table.ShowHeader = false;

        cap.table.AddLine(Cell{L"Options:", Format::Bright});
        cap.table.AddRow({L"name", L"desc"});
        cap.Render();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), lines.size());
        // The section header should contain VT sequences (Bright + Default reset)
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, lines[0].find(L"\x1b["));
        VERIFY_IS_TRUE(lines[0].find(L"Options:") != std::wstring::npos);
    }

    TEST_METHOD(TableData_AddLine_CellStrippedWhenVTDisabled)
    {
        using namespace wsl::windows::common::vt;
        TableCapture cap({L"", L""});
        cap.table.ShowHeader = false;

        cap.table.AddLine(Cell{L"Options:", Format::Bright});
        cap.table.AddRow({L"name", L"desc"});
        cap.Render();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), lines.size());
        // VT disabled: no escape sequences, just the text
        VERIFY_ARE_EQUAL(std::wstring::npos, lines[0].find(L"\x1b["));
        VERIFY_ARE_EQUAL(std::wstring{L"Options:"}, lines[0]);
    }
};

} // namespace WSLCCLITableDataUnitTests
