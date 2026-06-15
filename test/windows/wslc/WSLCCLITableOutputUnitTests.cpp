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
#include "VTSupport.h"

using namespace wsl::windows::wslc;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLITableOutputUnitTests {

class WSLCCLITableOutputUnitTests
{
    WSLC_TEST_CLASS(WSLCCLITableOutputUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(TableOutput_AlwaysShowHeader_EmitsHeaderWhenEmpty)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetAlwaysShowHeader(true);
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_NoHeader_EmitsNothingWhenEmpty)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetAlwaysShowHeader(false);
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines().size());
    }

    TEST_METHOD(TableOutput_SingleRow_EmitsHeaderPlusOneDataLine)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

        cap.table.OutputLine({L"my-container", L"running"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"my-container") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"running") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_MultipleRows_AllRowsEmittedAfterHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

        cap.table.OutputLine({L"container-a", L"running"});
        cap.table.OutputLine({L"container-b", L"stopped"});
        cap.table.OutputLine({L"container-c", L"paused"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(4), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[1].find(L"container-a") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[2].find(L"container-b") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[3].find(L"container-c") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ColumnPadding_DefaultPaddingApplied)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, /*sizingBuffer=*/50, /*columnPadding=*/3);

        cap.table.OutputLine({L"abc", L"ok"});
        cap.table.Complete();

        const std::wstring& dataLine = cap.lines()[1];
        VERIFY_IS_TRUE(dataLine.find(L"abc") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"ok") != std::wstring::npos);

        auto columnPadding = 3;
        auto namePos = dataLine.find(L"abc");
        auto statusPos = dataLine.find(L"ok");
        VERIFY_IS_TRUE(statusPos >= namePos + wcslen(L"abc") + columnPadding);
    }

    TEST_METHOD(TableOutput_ColumnPadding_CustomPaddingApplied)
    {
        constexpr size_t customPadding = 5;
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"A", L"B"}, /*sizingBuffer=*/50, customPadding);

        cap.table.OutputLine({L"x", L"y"});
        cap.table.Complete();

        const std::wstring& dataLine = cap.lines()[1];
        auto posX = dataLine.find(L'x');
        auto posY = dataLine.find(L'y');
        VERIFY_IS_TRUE(posX != std::wstring::npos);
        VERIFY_IS_TRUE(posY != std::wstring::npos);
        VERIFY_IS_TRUE(posY >= posX + 1 + customPadding);
    }

    TEST_METHOD(TableOutput_ColumnWidth_ExpandsToFitData)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"ID", L"NAME"});

        cap.table.OutputLine({L"1", L"short"});
        cap.table.OutputLine({L"2", L"a-very-long-container-name"});
        cap.table.Complete();

        VERIFY_IS_TRUE(cap.lines()[2].find(L"a-very-long-container-name") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ColumnWidth_AtLeastHeaderWidth)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"CONTAINER_NAME", L"ST"});

        cap.table.OutputLine({L"abc", L"ok"});
        cap.table.Complete();

        const std::wstring& dataLine = cap.lines()[1];
        auto posOk = dataLine.find(L"ok");
        VERIFY_IS_TRUE(posOk != std::wstring::npos);
        VERIFY_IS_TRUE(posOk >= static_cast<size_t>(14 + TableOutput<2>::DefaultColumnPadding));
    }

    TEST_METHOD(TableOutput_MaxWidth_LongValueIsTruncatedWithEllipsis)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 8;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, std::move(configs));

        cap.table.OutputLine({L"a-very-long-name", L"running"});
        cap.table.Complete();

        const std::wstring& dataLine = cap.lines()[1];
        VERIFY_IS_TRUE(dataLine.find(L"\x2026") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"a-very-long-name") == std::wstring::npos);
    }

    TEST_METHOD(TableOutput_MaxWidth_ShortValueNotTruncated)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 20;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, std::move(configs));

        cap.table.OutputLine({L"short", L"running"});
        cap.table.Complete();

        const std::wstring& dataLine = cap.lines()[1];
        VERIFY_IS_TRUE(dataLine.find(L"short") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"\x2026") == std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ConsoleWidthLimit_PreferredShrinkColumnIsShrunk)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = ColumnWidthConfig::NoLimit;
        configs[0].Overflow = ColumnOverflow::Shrink;
        configs[0].PreferredShrink = false;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;
        configs[1].Overflow = ColumnOverflow::Shrink;
        configs[1].PreferredShrink = true;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"ID", L"DESCRIPTION"}, std::move(configs));
        cap.table.SetConsoleWidthOverride(20);

        cap.table.OutputLine({L"abc123", L"this-is-a-long-description-value"});
        cap.table.Complete();

        for (const auto& line : cap.lines())
        {
            VERIFY_IS_TRUE(line.size() <= static_cast<size_t>(20));
        }
    }

    TEST_METHOD(TableOutput_IsEmpty)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        VERIFY_IS_TRUE(cap.table.IsEmpty());

        cap.table.OutputLine({L"foo", L"bar"});
        VERIFY_IS_FALSE(cap.table.IsEmpty());
    }

    TEST_METHOD(TableOutput_ColumnDefinition_NameAndConfigUsed)
    {
        TableOutput<2>::column_def_t defs{{
            ColumnDefinition{L"MYID", {.MinWidth = ColumnWidthConfig::NoLimit, .MaxWidth = 6, .Overflow = ColumnOverflow::Shrink, .PreferredShrink = false}},
            ColumnDefinition{L"MYNAME", {.MinWidth = ColumnWidthConfig::NoLimit, .MaxWidth = ColumnWidthConfig::NoLimit}},
        }};

        TableOutputCapture<2> cap(std::move(defs));

        cap.table.OutputLine({L"id-value", L"name-value"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"MYID") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"MYNAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"\x2026") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ShowHeader_False_SuppressesHeaderWithDataRows)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"my-container", L"running"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"my-container") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ShowHeader_False_SuppressesHeaderEvenWhenAlwaysShowHeaderTrue)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetAlwaysShowHeader(true);
        cap.table.SetShowHeader(false);
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines().size());
    }

    TEST_METHOD(TableOutput_ShowHeader_True_IsDefaultAndEmitsHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

        cap.table.OutputLine({L"my-container", L"running"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ShowHeader_False_MultipleDataRowsNoHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"container-a", L"running"});
        cap.table.OutputLine({L"container-b", L"stopped"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"container-a") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"container-b") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"NAME") == std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ShowHeader)
    {
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

            cap.table.OutputLine({L"my-container", L"running"});
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
            VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
        }
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
            cap.table.SetShowHeader(false);

            cap.table.OutputLine({L"my-container", L"running"});
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
            VERIFY_IS_TRUE(cap.lines()[0].find(L"my-container") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
        }
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
            cap.table.SetAlwaysShowHeader(true);
            cap.table.SetShowHeader(false);
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(0), cap.lines().size());
        }
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
            cap.table.SetShowHeader(false);

            cap.table.OutputLine({L"container-a", L"running"});
            cap.table.OutputLine({L"container-b", L"stopped"});
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
            VERIFY_IS_TRUE(cap.lines()[0].find(L"container-a") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[1].find(L"container-b") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") == std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[1].find(L"NAME") == std::wstring::npos);
        }
    }

    TEST_METHOD(TableOutput_RowIndent_PrependedToEveryRow)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetRowIndent(2);

        cap.table.OutputLine({L"abc", L"ok"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"  NAME   STATUS"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"  abc    ok"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_ShortValueProducesOneRow)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"opt", L"short desc"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   short desc"}, cap.lines()[0]);
    }

    TEST_METHOD(TableOutput_WordWrap_WrapsAtWordBoundary)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"opt", L"hello world"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_ContinuationRowHasBlankLeadingColumns)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"opt", L"hello world"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"      world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_MultipleWrapsProduceMultipleRows)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"opt", L"one two three four"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   one two"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      three four"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_HardBreakWhenNoSpaceFound)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 6;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"opt", L"abcdefghij"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   abcdef"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      ghij"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_NonWrappingColumnStillTruncates)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 5;
        // ColumnOverflow::Truncate (default) — truncates
        configs[1].MaxWidth = 20;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"toolongname", L"short desc"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"tool\u2026   short desc"}, cap.lines()[0]);
    }

    TEST_METHOD(TableOutput_WordWrap_RowIndentAppliedToAllPhysicalRows)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);
        cap.table.SetRowIndent(2);

        cap.table.OutputLine({L"opt", L"hello world"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"  opt   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"        world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_DisabledByDefault_LongTextTruncated)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 4;
        configs[1].MaxWidth = 8;
        // ColumnOverflow::Truncate (default) — truncates

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"opt", L"a very long description"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   a very \u2026"}, cap.lines()[0]);
    }

    TEST_METHOD(TableOutput_WordWrap_MultipleLogicalRowsEachWrapIndependently)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 10;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"opt-a", L"hello world"});
        cap.table.OutputLine({L"opt-b", L"short"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt-a   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"        world"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"opt-b   short"}, cap.lines()[2]);
    }

    TEST_METHOD(TableOutput_WordWrap_TwoWrappingColumnsColBLonger)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 5;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"ab cd ef", L"one two three"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"ab cd   one"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"ef      two"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"        three"}, cap.lines()[2]);
    }

    TEST_METHOD(TableOutput_WordWrap_TwoWrappingColumnsColALonger)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 5;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"one two three", L"ab cd ef"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"one     ab cd"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"two     ef"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"three   "}, cap.lines()[2]);
    }

    TEST_METHOD(TableOutput_WordWrap_TwoWrappingColumnsWithEqualLengths)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 4;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 4;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"aa bb", L"xx yy"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"aa     xx"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"bb     yy"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_NonWrappingColumnBetweenTwoWrappingColumns)
    {
        TableOutput<3>::column_config_t configs{};
        configs[0].MaxWidth = 4;
        // configs[0].Overflow = ColumnOverflow::Truncate (default) — truncates
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;
        configs[2].MaxWidth = 5;
        configs[2].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<3> cap(TableOutput<3>::header_t{L"", L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"tag", L"aa bb", L"xx yy zz"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"tag   aa bb   xx yy"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"              zz"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_WordWrap_FirstColumnLongerThanSecond)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 5;
        configs[0].Overflow = ColumnOverflow::Wrap;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);

        cap.table.OutputLine({L"one two three", L"ab cd ef"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"one     ab cd"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"two     ef"}, cap.lines()[1]);
        VERIFY_ARE_EQUAL(std::wstring{L"three   "}, cap.lines()[2]);
    }

    TEST_METHOD(TableOutput_SetColumnConfig_WordWrapAfterConstruction)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""});
        cap.table.SetShowHeader(false);
        cap.table.SetColumnConfig(
            1,
            ColumnWidthConfig{
                .MaxWidth = 8,
                .Overflow = ColumnOverflow::Wrap,
            });

        cap.table.OutputLine({L"opt", L"hello world"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_GetVisibleWidth_ExcludesCSISequences)
    {
        const std::wstring text = L"\x1b[91mhello\x1b[0m";
        VERIFY_ARE_EQUAL(static_cast<size_t>(5), GetVisibleWidth(text));
    }

    TEST_METHOD(TableOutput_GetVisibleWidth_ExcludesOSCSequences)
    {
        const std::wstring text = L"\x1b]8;;https://example.com\x1b\\text\x1b]8;;\x1b\\";
        VERIFY_ARE_EQUAL(static_cast<size_t>(4), GetVisibleWidth(text));
    }

    TEST_METHOD(TableOutput_ColorEnabled_ColumnWidthBasedOnVisibleChars)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);
        cap.table.SetColorEnabled(true);

        cap.table.OutputLine({L"opt", L"\x1b[91mhello\x1b[0m"});
        cap.table.OutputLine({L"end", L"world"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   \x1b[91mhello\x1b[0m"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"end   world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_ColorCell_UnmodifiedWhenFits)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);
        cap.table.SetColorEnabled(true);

        const std::wstring cell = Format::Fg::BrightRed + L"hello" + Format::Default;
        cap.table.OutputLine({L"opt", cell});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   \x1b[91mhello\x1b[0m"}, cap.lines()[0]);
    }

    TEST_METHOD(TableOutput_ColorCell_WrapsWithSequencesOnEachChunk)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);
        cap.table.SetColorEnabled(true);

        const std::wstring cell = Format::Fg::BrightRed + L"hello world" + Format::Default;
        cap.table.OutputLine({L"opt", cell});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   \x1b[91mhello\x1b[0m"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      \x1b[91mworld\x1b[0m"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_ColorCell_TruncatesVisibleTextOnly)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 5;
        // ColumnOverflow::Truncate (default) — truncates

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);
        cap.table.SetColorEnabled(true);

        const std::wstring cell = Format::Fg::BrightRed + L"hello world" + Format::Default;
        cap.table.OutputLine({L"opt", cell});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   \x1b[91mhell\u2026\x1b[0m"}, cap.lines()[0]);
    }

    TEST_METHOD(TableOutput_ColorCell_MultipleLeadingSequencesPreservedOnWrap)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);
        cap.table.SetColorEnabled(true);

        const std::wstring cell = Format::Bright + Format::Fg::BrightRed + L"hi there" + Format::Default;
        cap.table.OutputLine({L"opt", cell});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   \x1b[1m\x1b[91mhi\x1b[0m"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      \x1b[1m\x1b[91mthere\x1b[0m"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_ColorCell_MissingResetIsEnforcedOnWrap)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs));
        cap.table.SetShowHeader(false);
        cap.table.SetColorEnabled(true);

        const std::wstring cell = Format::Fg::BrightRed + L"hello world"; // no reset
        cap.table.OutputLine({L"opt", cell});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   \x1b[91mhello\x1b[0m"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      \x1b[91mworld\x1b[0m"}, cap.lines()[1]);
    }
};

} // namespace WSLCCLITableOutputUnitTests
