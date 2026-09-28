// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

namespace p9fs {

// Class to help construct tracelogging messages for verbose logging of server traffic.
// N.B. To avoid needlessly increasing the size of the statically linked WSL init binary, this
//      helper allows for constructing log messages without the use of printf or iostream.
class LogMessageBuilder
{
public:
    void AddName(std::string_view name);
    void AddField(std::string_view name, std::string_view value);
    void AddField(std::string_view name, UINT64 value, int base = 10);
    void AddField(std::string_view name, const Qid& value);
    void AddValue(std::string_view value);
    void AddValue(const Qid& qid);

    const char* String() const;

private:
    void AddFieldName(std::string_view name);
    void AddRawValue(UINT64 value, int base = 10);
    void AddRawValue(const Qid& value);
    void AddRawValue(std::string_view value);

    std::string m_message;
};

} // namespace p9fs
