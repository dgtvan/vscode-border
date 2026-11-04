#include "codex_usage.h"
#include "codex_usage_protocol.h"
#include "text_util.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace {
// Validate the wire JSON before using the repo's JSONC span scanner. A
// truncated object must never accidentally produce a valid-looking 0%.
class JsonReader {
    const std::string& s;
    size_t i = 0;
    void Space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; }
    bool Take(char c) { Space(); if (i == s.size() || s[i] != c) return false; ++i; return true; }
    bool String() {
        if (!Take('"')) return false;
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i++];
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (i == s.size()) return false;
            c = (unsigned char)s[i++];
            if (c == 'u') {
                for (int n = 0; n < 4; ++n) {
                    if (i == s.size() || std::string("0123456789abcdefABCDEF").find(s[i++]) == std::string::npos)
                        return false;
                }
            } else if (std::string("\"\\/bfnrt").find((char)c) == std::string::npos) return false;
        }
        return false;
    }
    bool Digits() { size_t start = i; while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i; return i > start; }
    bool Value(int depth) {
        if (depth > 64) return false;
        Space();
        if (i == s.size()) return false;
        char c = s[i];
        if (c == '"') return String();
        if (c == '{' || c == '[') {
            ++i;
            char close = c == '{' ? '}' : ']';
            if (Take(close)) return true;
            do {
                if (c == '{' && (!String() || !Take(':'))) return false;
                if (!Value(depth + 1)) return false;
                if (Take(close)) return true;
            } while (Take(','));
            return false;
        }
        for (const char* literal : {"true", "false", "null"}) {
            size_t n = std::char_traits<char>::length(literal);
            if (s.compare(i, n, literal) == 0) { i += n; return true; }
        }
        if (s[i] == '-') ++i;
        if (i == s.size()) return false;
        if (s[i] == '0') ++i;
        else if (!Digits()) return false;
        if (i < s.size() && s[i] == '.') { ++i; if (!Digits()) return false; }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!Digits()) return false;
        }
        return true;
    }
public:
    explicit JsonReader(const std::string& json) : s(json) {}
    bool Object() {
        Space();
        if (i == s.size() || s[i] != '{' || !Value(0)) return false;
        Space();
        return i == s.size();
    }
};
}

namespace CodexJson {
JsonScan::Span Root(const std::string& json) {
    if (!JsonReader(json).Object()) return {};
    size_t start = JsonScan::SkipWhitespaceAndComments(json, 0);
    if (start >= json.size() || json[start] != '{') return {};
    size_t end = JsonScan::FindValueEnd(json, start);
    if (end <= start + 1 || end > json.size() || json[end - 1] != '}' ||
        JsonScan::SkipWhitespaceAndComments(json, end) != json.size()) return {};
    return {true, start + 1, end - 1};
}

JsonScan::Span Object(const std::string& json, const JsonScan::Span& parent, const char* key) {
    if (!parent.found) return {};
    auto e = JsonScan::FindKeyInObject(json, parent.start, parent.end, key);
    if (!e.found || e.valueEnd <= e.valueStart + 1 || e.valueEnd > json.size() ||
        json[e.valueStart] != '{' || json[e.valueEnd - 1] != '}') return {};
    return {true, e.valueStart + 1, e.valueEnd - 1};
}

std::string Raw(const std::string& json, const JsonScan::Span& parent, const char* key) {
    if (!parent.found) return "";
    auto e = JsonScan::FindKeyInObject(json, parent.start, parent.end, key);
    return e.found ? json.substr(e.valueStart, e.valueEnd - e.valueStart) : "";
}

std::wstring String(const std::string& raw) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return L"";
    std::wstring out;
    std::string run;
    auto flush = [&]() { out += Utf8ToWide(run); run.clear(); };
    for (size_t i = 1; i + 1 < raw.size(); i++) {
        if (raw[i] != '\\') { run += raw[i]; continue; }
        if (++i + 1 >= raw.size()) return L"";
        char c = raw[i];
        if (c == 'u') {
            if (i + 4 >= raw.size() - 1) return L"";
            std::string hex = raw.substr(i + 1, 4);
            if (hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return L"";
            flush();
            out += (wchar_t)strtoul(hex.c_str(), nullptr, 16);
            i += 4;
        } else {
            if (std::string("\"\\/ntrbf").find(c) == std::string::npos) return L"";
            run += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
        }
    }
    flush();
    return out;
}
} // namespace CodexJson

namespace {
bool Number(const std::string& raw, double& out) {
    if (raw.empty() || (raw[0] != '-' && (raw[0] < '0' || raw[0] > '9'))) return false;
    char* end = nullptr;
    out = strtod(raw.c_str(), &end);
    return end == raw.c_str() + raw.size() && std::isfinite(out);
}

bool ParseLimit(const std::string& json, const JsonScan::Span& bucket, const char* key,
                CodexUsageLimit& out, std::wstring& error) {
    using namespace CodexJson;
    std::string raw = Raw(json, bucket, key);
    if (raw.empty() || raw == "null") return true;
    auto obj = Object(json, bucket, key);
    double n;
    if (!obj.found || !Number(Raw(json, obj, "usedPercent"), n) || n < 0 || n > 100) {
        error = L"Codex returned an invalid usedPercent; usage is unavailable";
        return false;
    }
    out.percent = n;
    raw = Raw(json, obj, "resetsAt");
    if (!raw.empty() && raw != "null") {
        // Keep timestamps inside the range supported by Windows time conversion.
        if (!Number(raw, n) || n < 0 || n > 253402300799.0 || std::floor(n) != n) {
            error = L"Codex returned an invalid reset timestamp";
            return false;
        }
        out.resetsAt = (long long)n;
    }
    raw = Raw(json, obj, "windowDurationMins");
    if (!raw.empty() && raw != "null") {
        if (!Number(raw, n) || n <= 0 || n > std::numeric_limits<int>::max() || std::floor(n) != n) {
            error = L"Codex returned an invalid usage window duration";
            return false;
        }
        out.windowMinutes = (int)n;
    }
    return true;
}
}

bool ParseCodexRateLimits(const std::string& result, CodexUsageSnapshot& snapshot, std::wstring& error) {
    using namespace CodexJson;
    error.clear();
    auto root = Root(result);
    auto byId = Object(result, root, "rateLimitsByLimitId");
    auto bucket = Object(result, byId, "codex");
    if (!bucket.found) {
        bucket = Object(result, root, "rateLimits");
        std::string id = Raw(result, bucket, "limitId");
        if (!id.empty() && id != "null" && String(id) != L"codex") bucket = {};
    }
    if (!bucket.found) {
        error = L"Codex did not report account usage limits (a ChatGPT login is required)";
        return false;
    }
    CodexUsageLimit primary, secondary;
    if (!ParseLimit(result, bucket, "primary", primary, error) ||
        !ParseLimit(result, bucket, "secondary", secondary, error)) return false;
    if (primary.percent < 0 && secondary.percent < 0) {
        error = L"Codex did not report either usage window for this account";
        return false;
    }
    snapshot.primary = primary;
    snapshot.secondary = secondary;
    return true;
}
