#include "json_scan.h"

namespace JsonScan {

size_t SkipWhitespaceAndComments(const std::string& s, size_t i) {
    size_t n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        break;
    }
    return i;
}

size_t FindValueEnd(const std::string& s, size_t start) {
    size_t n = s.size();
    size_t i = start;
    int depth = 0;
    size_t lastNonWs = start;
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (c == '"') {
            i++;
            while (i < n) {
                if (s[i] == '\\') { i += 2; continue; }
                if (s[i] == '"') { i++; break; }
                i++;
            }
            lastNonWs = i;
            continue;
        }
        if (c == '{' || c == '[') { depth++; i++; lastNonWs = i; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0) break;
            depth--; i++; lastNonWs = i; continue;
        }
        if (c == ',' && depth == 0) break;
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) lastNonWs = i + 1;
        i++;
    }
    return lastNonWs;
}

size_t FindRootObjectOpenBrace(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (c == '{') return i;
        i++;
    }
    return std::string::npos;
}

Span EnsureRootObject(std::string& content) {
    size_t brace = FindRootObjectOpenBrace(content);
    if (brace == std::string::npos) {
        content = "{\n}\n";
        brace = 0;
    }
    // FindValueEnd on a value starting with '{' consumes through its own
    // matching close brace and returns one past it (see its own comment).
    size_t afterClose = FindValueEnd(content, brace);
    return {true, brace + 1, afterClose - 1};
}

KeyEntry FindKeyInObject(const std::string& content, size_t objStart, size_t objEnd, const std::string& key) {
    KeyEntry result;
    int depth = 0;
    size_t i = objStart;
    std::string quotedKey = "\"" + key + "\"";

    while (i < objEnd) {
        char c = content[i];
        if (c == '/' && i + 1 < objEnd && content[i + 1] == '/') {
            i += 2;
            while (i < objEnd && content[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < objEnd && content[i + 1] == '*') {
            i += 2;
            while (i + 1 < objEnd && !(content[i] == '*' && content[i + 1] == '/')) i++;
            i = (i + 1 < objEnd) ? i + 2 : objEnd;
            continue;
        }
        if (c == '"') {
            size_t stringStart = i;
            i++;
            while (i < objEnd) {
                if (content[i] == '\\') { i += 2; continue; }
                if (content[i] == '"') { i++; break; }
                i++;
            }
            if (depth == 0 && content.compare(stringStart, i - stringStart, quotedKey) == 0) {
                size_t j = SkipWhitespaceAndComments(content, i);
                if (j < objEnd && content[j] == ':') {
                    j = SkipWhitespaceAndComments(content, j + 1);
                    result.found = true;
                    result.keyStart = stringStart;
                    result.valueStart = j;
                    result.valueEnd = FindValueEnd(content, j);
                    return result;
                }
            }
            continue;
        }
        if (c == '{' || c == '[') { depth++; i++; continue; }
        if (c == '}' || c == ']') { depth--; i++; continue; }
        i++;
    }
    return result;
}

std::vector<Span> FindArrayElements(const std::string& content, size_t arrStart, size_t arrEnd) {
    std::vector<Span> elements;
    size_t i = SkipWhitespaceAndComments(content, arrStart);
    while (i < arrEnd) {
        size_t elemEnd = FindValueEnd(content, i);
        if (elemEnd <= i) break; // malformed/empty -- stop rather than loop forever
        elements.push_back({true, i, elemEnd});
        i = SkipWhitespaceAndComments(content, elemEnd);
        if (i < arrEnd && content[i] == ',') {
            i = SkipWhitespaceAndComments(content, i + 1);
        } else {
            break;
        }
    }
    return elements;
}

// Shared by RemoveObjectKey/RemoveArrayElement: removes [spanStart, spanEnd)
// plus whichever adjacent comma/whitespace keeps the container valid.
static std::string RemoveSpanWithComma(const std::string& content, size_t spanStart, size_t spanEnd) {
    size_t removeStart = spanStart;
    size_t removeEnd = spanEnd;

    size_t j = SkipWhitespaceAndComments(content, removeEnd);
    bool ateTrailingComma = false;
    if (j < content.size() && content[j] == ',') {
        removeEnd = j + 1;
        ateTrailingComma = true;
    }

    while (removeStart > 0 && (content[removeStart - 1] == ' ' || content[removeStart - 1] == '\t')) removeStart--;

    if (ateTrailingComma) {
        if (removeStart > 0 && content[removeStart - 1] == '\n') {
            removeStart--;
            if (removeStart > 0 && content[removeStart - 1] == '\r') removeStart--;
        }
    } else {
        size_t k = removeStart;
        while (k > 0 && (content[k - 1] == ' ' || content[k - 1] == '\t' ||
                         content[k - 1] == '\r' || content[k - 1] == '\n')) {
            k--;
        }
        if (k > 0 && content[k - 1] == ',') removeStart = k - 1;
    }
    return content.substr(0, removeStart) + content.substr(removeEnd);
}

std::string RemoveObjectKey(const std::string& content, const KeyEntry& entry) {
    return RemoveSpanWithComma(content, entry.keyStart, entry.valueEnd);
}

std::string RemoveArrayElement(const std::string& content, const Span& element) {
    return RemoveSpanWithComma(content, element.start, element.end);
}

std::string InsertIntoObject(std::string content, size_t objStart, size_t objEnd, const std::string& key,
                             const std::string& valueJson) {
    size_t afterOpen = SkipWhitespaceAndComments(content, objStart);
    bool emptyObject = afterOpen >= objEnd;
    std::string insertion = "\n    \"" + key + "\": " + valueJson + (emptyObject ? "" : ",");
    content.insert(objStart, insertion);
    return content;
}

std::string InsertIntoArray(std::string content, size_t arrStart, size_t arrEnd, const std::string& elementJson) {
    size_t afterOpen = SkipWhitespaceAndComments(content, arrStart);
    bool emptyArray = afterOpen >= arrEnd;
    std::string insertion = "\n    " + elementJson + (emptyArray ? "" : ",");
    content.insert(arrStart, insertion);
    return content;
}

} // namespace JsonScan
