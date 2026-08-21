#include "project_list_order.h"

#include "file_util.h"
#include "text_util.h"

namespace {

std::wstring GetOrderFilePath() {
    return GetExeDir() + L"\\project_list_order.ini";
}

} // namespace

std::vector<std::wstring> LoadItemOrder() {
    std::vector<std::wstring> order;
    std::string content = ReadFileBytes(GetOrderFilePath());

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (eol == std::string::npos) pos = content.size() + 1;
        else pos = eol + 1;
        if (line.empty()) continue;

        order.push_back(UnescapeTextLine(line));
    }
    return order;
}

void SaveItemOrder(const std::vector<std::wstring>& order) {
    std::string out;
    for (const std::wstring& label : order) {
        out += EscapeTextLine(label) + "\n";
    }
    WriteFileBytes(GetOrderFilePath(), out);
}
