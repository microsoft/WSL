/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TablePrinter.h

Abstract:

    This file contains the TablePrinter header implementation

--*/
#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <wslutil.h>

namespace wslc
{

class TablePrinter
{
public:
    explicit TablePrinter(const std::vector<std::wstring>& headers)
        : m_headers(headers),
          m_widths(headers.size(), 0) {}

    TablePrinter(std::initializer_list<std::wstring> headers)
        : m_headers(headers),
          m_widths(headers.size(), 0) {}

    void AddRow(const std::vector<std::wstring>& row)
    {
        if (row.size() != m_headers.size()) {
            THROW_HR(E_INVALIDARG);
        }
        m_rows.push_back(row);
    }

    void Print()
    {
        ComputeWidths();
        PrintSeparator();
        PrintRow(m_headers);
        PrintSeparator();
        for (const auto& row : m_rows)
        {
            PrintRow(row);
        }
        PrintSeparator();
    }

private:
    std::vector<std::wstring> m_headers;
    std::vector<std::vector<std::wstring>> m_rows;
    std::vector<size_t> m_widths;

    void ComputeWidths()
    {
        for (size_t i = 0; i < m_headers.size(); ++i) {
            m_widths[i] = m_headers[i].size();
            for (const auto& row : m_rows) {
                m_widths[i] = std::max(m_widths[i], row[i].size());
            }
        }
    }

    void PrintSeparator() const
    {
        std::wstringstream ss;
        ss << "+";
        for (size_t w : m_widths) {
            ss << std::wstring(w + 2, '-') << "+";
        }
        wsl::windows::common::wslutil::PrintMessage(ss.str(), stdout);
    }

    void PrintRow(const std::vector<std::wstring>& row) const
    {
        std::wstringstream ss;
        ss << "|";
        for (size_t i = 0; i < row.size(); ++i) {
            ss << " " << row[i]
               << std::wstring(m_widths[i] - row[i].size(), ' ')
               << " |";
        }
        wsl::windows::common::wslutil::PrintMessage(ss.str(), stdout);
    }
};
}
