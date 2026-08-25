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
using namespace wsl::windows::common::vt;
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

        cap.table.WriteRow({L"my-container", L"running"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"my-container") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[1].find(L"running") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_MultipleRows_AllRowsEmittedAfterHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});

        cap.table.WriteRow({L"container-a", L"running"});
        cap.table.WriteRow({L"container-b", L"stopped"});
        cap.table.WriteRow({L"container-c", L"paused"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(4), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[1].find(L"container-a") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[2].find(L"container-b") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[3].find(L"container-c") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ColumnPadding_DefaultPaddingApplied)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, /*sizingBuffer=*/50, /*columnPadding=*/3);

        cap.table.WriteRow({L"abc", L"ok"});
        cap.table.Complete();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
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

        cap.table.WriteRow({L"x", L"y"});
        cap.table.Complete();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
        auto posX = dataLine.find(L'x');
        auto posY = dataLine.find(L'y');
        VERIFY_IS_TRUE(posX != std::wstring::npos);
        VERIFY_IS_TRUE(posY != std::wstring::npos);
        VERIFY_IS_TRUE(posY >= posX + 1 + customPadding);
    }

    TEST_METHOD(TableOutput_ColumnWidth_ExpandsToFitData)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"ID", L"NAME"});

        cap.table.WriteRow({L"1", L"short"});
        cap.table.WriteRow({L"2", L"a-very-long-container-name"});
        cap.table.Complete();

        VERIFY_IS_TRUE(cap.lines()[2].find(L"a-very-long-container-name") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ColumnWidth_AtLeastHeaderWidth)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"CONTAINER_NAME", L"ST"});

        cap.table.WriteRow({L"abc", L"ok"});
        cap.table.Complete();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
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

        cap.table.WriteRow({L"a-very-long-name", L"running"});
        cap.table.Complete();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
        VERIFY_IS_TRUE(dataLine.find(L"\x2026") != std::wstring::npos);
        VERIFY_IS_TRUE(dataLine.find(L"a-very-long-name") == std::wstring::npos);
    }

    TEST_METHOD(TableOutput_MaxWidth_ShortValueNotTruncated)
    {
        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 20;
        configs[1].MaxWidth = ColumnWidthConfig::NoLimit;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"}, std::move(configs));

        cap.table.WriteRow({L"short", L"running"});
        cap.table.Complete();

        auto lines = cap.lines();
        const auto& dataLine = lines[1];
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

        cap.table.WriteRow({L"abc123", L"this-is-a-long-description-value"});
        cap.table.Complete();

        auto lines = cap.lines();
        for (const auto& line : lines)
        {
            VERIFY_IS_TRUE(line.size() <= static_cast<size_t>(20));
        }
    }

    TEST_METHOD(TableOutput_IsEmpty)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        VERIFY_IS_TRUE(cap.table.IsEmpty());

        cap.table.WriteRow({L"foo", L"bar"});
        VERIFY_IS_FALSE(cap.table.IsEmpty());
    }

    TEST_METHOD(TableOutput_ColumnDefinition_NameAndConfigUsed)
    {
        TableOutput<2>::column_def_t defs{{
            ColumnDefinition{L"MYID", {.MinWidth = ColumnWidthConfig::NoLimit, .MaxWidth = 6, .Overflow = ColumnOverflow::Shrink, .PreferredShrink = false}},
            ColumnDefinition{L"MYNAME", {.MinWidth = ColumnWidthConfig::NoLimit, .MaxWidth = ColumnWidthConfig::NoLimit}},
        }};

        TableOutputCapture<2> cap(std::move(defs));

        cap.table.WriteRow({L"id-value", L"name-value"});
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

        cap.table.WriteRow({L"my-container", L"running"});
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

        cap.table.WriteRow({L"my-container", L"running"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
        VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_ShowHeader_False_MultipleDataRowsNoHeader)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"container-a", L"running"});
        cap.table.WriteRow({L"container-b", L"stopped"});
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

            cap.table.WriteRow({L"my-container", L"running"});
            cap.table.Complete();

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
            VERIFY_IS_TRUE(cap.lines()[0].find(L"NAME") != std::wstring::npos);
            VERIFY_IS_TRUE(cap.lines()[0].find(L"STATUS") != std::wstring::npos);
        }
        {
            TableOutputCapture<2> cap(TableOutput<2>::header_t{L"NAME", L"STATUS"});
            cap.table.SetShowHeader(false);

            cap.table.WriteRow({L"my-container", L"running"});
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

            cap.table.WriteRow({L"container-a", L"running"});
            cap.table.WriteRow({L"container-b", L"stopped"});
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

        cap.table.WriteRow({L"abc", L"ok"});
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

        cap.table.WriteRow({L"opt", L"short desc"});
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

        cap.table.WriteRow({L"opt", L"hello world"});
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

        cap.table.WriteRow({L"opt", L"hello world"});
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

        cap.table.WriteRow({L"opt", L"one two three four"});
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

        cap.table.WriteRow({L"opt", L"abcdefghij"});
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

        cap.table.WriteRow({L"toolongname", L"short desc"});
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

        cap.table.WriteRow({L"opt", L"hello world"});
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

        cap.table.WriteRow({L"opt", L"a very long description"});
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

        cap.table.WriteRow({L"opt-a", L"hello world"});
        cap.table.WriteRow({L"opt-b", L"short"});
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

        cap.table.WriteRow({L"ab cd ef", L"one two three"});
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

        cap.table.WriteRow({L"one two three", L"ab cd ef"});
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

        cap.table.WriteRow({L"aa bb", L"xx yy"});
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

        cap.table.WriteRow({L"tag", L"aa bb", L"xx yy zz"});
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

        cap.table.WriteRow({L"one two three", L"ab cd ef"});
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

        cap.table.WriteRow({L"opt", L"hello world"});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        VERIFY_ARE_EQUAL(std::wstring{L"opt   hello"}, cap.lines()[0]);
        VERIFY_ARE_EQUAL(std::wstring{L"      world"}, cap.lines()[1]);
    }

    TEST_METHOD(FormattedCell_VisibleWidth_PlainText)
    {
        FormattedCell cell{L"hello"};
        VERIFY_ARE_EQUAL(static_cast<size_t>(5), cell.VisibleWidth());
    }

    TEST_METHOD(FormattedCell_VisibleWidth_ExcludesPlaceholders)
    {
        FormattedCell cell{L"{}hello{}", {&Format::Fg::BrightRed, &Format::Default}};
        VERIFY_ARE_EQUAL(static_cast<size_t>(5), cell.VisibleWidth());
    }

    TEST_METHOD(FormattedCell_Render_ColorEnabled)
    {
        FormattedCell cell = FormattedCell(L"hello", Format::Fg::BrightRed);
        const auto result = cell.Render(true, true);
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\x1b'));
    }

    TEST_METHOD(FormattedCell_Render_ColorDisabled)
    {
        FormattedCell cell = FormattedCell(L"hello", Format::Fg::BrightRed);
        const auto result = cell.Render(false, false);
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, result);
    }

    TEST_METHOD(FormattedCell_Render_VTEnabled_ColorDisabled_StripsColorSequences)
    {
        FormattedCell cell = FormattedCell(L"hello", Format::Fg::BrightRed);
        // VT on but color off: color sequences should be stripped
        const auto result = cell.Render(true, false);
        VERIFY_ARE_EQUAL(std::wstring{L"hello"}, result);
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L'\x1b'));
    }

    TEST_METHOD(FormattedCell_RenderTruncated_TruncatesVisibleText)
    {
        FormattedCell cell = FormattedCell(L"hello world", Format::Fg::BrightRed);
        const auto result = cell.RenderTruncated(5, true, true);
        // Should contain truncated visible text + ellipsis + sequences
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L"world"));
    }

    TEST_METHOD(TableOutput_FormattedCell_ColumnWidthBasedOnVisibleChars)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs), true);
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"opt", FormattedCell(L"hello", Format::Fg::BrightRed)});
        cap.table.WriteRow({L"end", FormattedCell{L"world"}});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        // First cell is emphasized (has ESC), second is plain
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring{L"end   world"}, cap.lines()[1]);
    }

    TEST_METHOD(TableOutput_FormattedCell_WrapsWithSequencesOnEachChunk)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs), true);
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"opt", FormattedCell(L"hello world", Format::Fg::BrightRed)});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        // Both lines should have emphasis sequences
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[1].find(L'\x1b'));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[1].find(L"world"));
    }

    TEST_METHOD(FormattedCell_DefaultCtor_EmptyCell)
    {
        FormattedCell cell;
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cell.VisibleWidth());
        VERIFY_ARE_EQUAL(std::wstring{L""}, cell.Render(true, true));
        VERIFY_ARE_EQUAL(std::wstring{L""}, cell.Render(false, false));
    }

    TEST_METHOD(FormattedCell_SingleSequenceCtor_BuildsFormatWithReset)
    {
        FormattedCell cell(L"bold", Format::Bright);
        VERIFY_ARE_EQUAL(static_cast<size_t>(4), cell.VisibleWidth());
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cell.sequences.size());

        // Color enabled: sequences emitted
        const auto rendered = cell.Render(true, true);
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, rendered.find(L"bold"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, rendered.find(L'\x1b'));

        // Color disabled: plain text only
        VERIFY_ARE_EQUAL(std::wstring{L"bold"}, cell.Render(false, false));
    }

    TEST_METHOD(FormattedCell_VisibleWidth_MultiplePlaceholders)
    {
        // "{}a{}bc{}" = 3 visible chars (a, b, c), 3 placeholders
        FormattedCell cell{L"{}a{}bc{}", {&Format::Bright, &Format::Fg::BrightRed, &Format::Default}};
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), cell.VisibleWidth());
    }

    TEST_METHOD(FormattedCell_VisibleWidth_EmptyFormat)
    {
        FormattedCell cell{L""};
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cell.VisibleWidth());
    }

    TEST_METHOD(FormattedCell_VisibleWidth_OnlyPlaceholders)
    {
        FormattedCell cell{L"{}{}", {&Format::Bright, &Format::Default}};
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), cell.VisibleWidth());
    }

    TEST_METHOD(FormattedCell_Render_PlainTextNoSequences)
    {
        FormattedCell cell{L"plain text"};
        VERIFY_ARE_EQUAL(std::wstring{L"plain text"}, cell.Render(true, true));
        VERIFY_ARE_EQUAL(std::wstring{L"plain text"}, cell.Render(false, false));
    }

    TEST_METHOD(FormattedCell_RenderTruncated_TextFitsNoTruncation)
    {
        FormattedCell cell(L"hello", Format::Fg::BrightRed);
        const auto result = cell.RenderTruncated(10, true, true);
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L"hello"));
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L'\u2026')); // no ellipsis
    }

    TEST_METHOD(FormattedCell_RenderTruncated_ColorDisabled_StillTruncates)
    {
        FormattedCell cell(L"hello world", Format::Fg::BrightRed);
        const auto result = cell.RenderTruncated(5, false, false);
        // Sequences stripped, but truncation still occurs
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L"world"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\u2026'));
    }

    TEST_METHOD(FormattedCell_RenderTruncated_PlainText)
    {
        FormattedCell cell{L"abcdefghij"};
        const auto result = cell.RenderTruncated(5, false, false);
        VERIFY_ARE_EQUAL(std::wstring::npos, result.find(L"fghij"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.find(L'\u2026'));
    }

    TEST_METHOD(TableOutput_FormattedCell_ColorDisabled_SequencesStripped)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 10;
        configs[1].MaxWidth = 20;

        // vtEnabled=false: sequences should be stripped from output
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs), false);
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"opt", FormattedCell(L"hello", Format::Fg::BrightRed)});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), cap.lines().size());
        // No ESC bytes in output
        VERIFY_ARE_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        // Visible text still present with correct padding
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
    }

    TEST_METHOD(TableOutput_FormattedCell_WrapColorDisabled_SequencesStripped)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs), false);
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"opt", FormattedCell(L"hello world", Format::Fg::BrightRed)});
        cap.table.Complete();

        VERIFY_ARE_EQUAL(static_cast<size_t>(2), cap.lines().size());
        // No ESC in either line
        VERIFY_ARE_EQUAL(std::wstring::npos, cap.lines()[0].find(L'\x1b'));
        VERIFY_ARE_EQUAL(std::wstring::npos, cap.lines()[1].find(L'\x1b'));
        // Text still wraps correctly
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[0].find(L"hello"));
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, cap.lines()[1].find(L"world"));
    }

    TEST_METHOD(TableOutput_FormattedCell_WrapMultipleLines_SequenceReapplied)
    {
        using namespace wsl::windows::common::vt;

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 5;
        configs[1].Overflow = ColumnOverflow::Wrap;

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs), true);
        cap.table.SetShowHeader(false);

        // Text that wraps into 3 lines: "aa bb cc" at width 5 -> "aa bb" / "cc"
        // Actually at width 5: "aa" "bb" "cc" (word-break at spaces)
        cap.table.WriteRow({L"cmd", FormattedCell(L"aa bb cc dd", Format::Fg::BrightCyan)});
        cap.table.Complete();

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

    TEST_METHOD(TableOutput_FormattedCell_HyperlinkTruncated_BothSequencesEmitted)
    {
        using namespace wsl::windows::common::vt;

        // Simulate a hyperlink cell: OSC 8 open + visible text + OSC 8 close.
        // The open/close are ConstructedSequences since they contain a URL.
        const ConstructedSequence hyperlinkOpen{L"\x1b]8;;https://example.com\x1b\\"};
        const ConstructedSequence hyperlinkClose{L"\x1b]8;;\x1b\\"};

        TableOutput<2>::column_config_t configs{};
        configs[0].MaxWidth = 6;
        configs[1].MaxWidth = 8; // Force truncation of "click here now" (14 chars)

        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, std::move(configs), true);
        cap.table.SetShowHeader(false);

        // FormattedCell: {}visible text{}  with hyperlink open/close as sequences
        cap.table.WriteRow({L"link", FormattedCell(L"{}click here now{}", {&hyperlinkOpen, &hyperlinkClose})});
        cap.table.Complete();

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

    TEST_METHOD(TableOutput_WriteLine_BlankLineEmittedBetweenRows)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""});
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"row-a", L"val-a"});
        cap.table.WriteLine();
        cap.table.WriteRow({L"row-b", L"val-b"});
        cap.table.Complete();

        auto lines = cap.lines();
        // 3 lines: data, blank, data
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), lines.size());
        VERIFY_IS_TRUE(lines[0].find(L"row-a") != std::wstring::npos);
        VERIFY_IS_TRUE(lines[1].empty());
        VERIFY_IS_TRUE(lines[2].find(L"row-b") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_WriteLine_SectionHeaderEmittedBetweenRows)
    {
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""});
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"opt-a", L"desc-a"});
        cap.table.WriteLine(FormattedCell{L"Global Options:"});
        cap.table.WriteRow({L"opt-b", L"desc-b"});
        cap.table.Complete();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), lines.size());
        VERIFY_IS_TRUE(lines[0].find(L"opt-a") != std::wstring::npos);
        VERIFY_ARE_EQUAL(std::wstring{L"Global Options:"}, lines[1]);
        VERIFY_IS_TRUE(lines[2].find(L"opt-b") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_WriteLine_DoesNotAffectColumnWidths)
    {
        // The break text is longer than any data cell; column widths should
        // be driven only by data rows, not breaks.
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""});
        cap.table.SetShowHeader(false);

        cap.table.WriteRow({L"ab", L"cd"});
        cap.table.WriteLine(FormattedCell{L"This is a very long section header that should not widen columns"});
        cap.table.WriteRow({L"ef", L"gh"});
        cap.table.Complete();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), lines.size());
        // Both data rows should have the same width (driven by "ab"/"ef" column)
        VERIFY_ARE_EQUAL(lines[0].size(), lines[2].size());
    }

    TEST_METHOD(TableOutput_WriteLine_SharedColumnWidthsAcrossSections)
    {
        // Data rows in different sections share column widths because they
        // are sized together within a single table instance.
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""});
        cap.table.SetShowHeader(false);

        cap.table.WriteLine(FormattedCell{L"Section A:"});
        cap.table.WriteRow({L"short", L"x"});
        cap.table.WriteLine(FormattedCell{L"Section B:"});
        cap.table.WriteRow({L"much-longer-name", L"y"});
        cap.table.Complete();

        auto lines = cap.lines();
        // 4 lines: header, data, header, data
        VERIFY_ARE_EQUAL(static_cast<size_t>(4), lines.size());

        // "short" row should be padded to match "much-longer-name" column width.
        // Both data rows have the same total width.
        VERIFY_ARE_EQUAL(lines[1].size(), lines[3].size());
    }

    TEST_METHOD(TableOutput_WriteLine_FormattedCellRendersSequences)
    {
        using namespace wsl::windows::common::vt;
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""}, TableOutput<2>::column_config_t{}, true);
        cap.table.SetShowHeader(false);

        cap.table.WriteLine(FormattedCell{L"Options:", Format::Bright});
        cap.table.WriteRow({L"name", L"desc"});
        cap.table.Complete();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), lines.size());
        // The section header should contain VT sequences (Bright + Default reset)
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, lines[0].find(L"\x1b["));
        VERIFY_IS_TRUE(lines[0].find(L"Options:") != std::wstring::npos);
    }

    TEST_METHOD(TableOutput_WriteLine_FormattedCellStrippedWhenVTDisabled)
    {
        using namespace wsl::windows::common::vt;
        TableOutputCapture<2> cap(TableOutput<2>::header_t{L"", L""});
        cap.table.SetShowHeader(false);

        cap.table.WriteLine(FormattedCell{L"Options:", Format::Bright});
        cap.table.WriteRow({L"name", L"desc"});
        cap.table.Complete();

        auto lines = cap.lines();
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), lines.size());
        // VT disabled: no escape sequences, just the text
        VERIFY_ARE_EQUAL(std::wstring::npos, lines[0].find(L"\x1b["));
        VERIFY_ARE_EQUAL(std::wstring{L"Options:"}, lines[0]);
    }
};

} // namespace WSLCCLITableOutputUnitTests
