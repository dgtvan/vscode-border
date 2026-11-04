#pragma once

#include "json_scan.h"
#include <string>

// Small readers for the JSON objects in Codex's JSONL protocol. They never
// synthesize an object for malformed input (unlike EnsureRootObject).
namespace CodexJson {
JsonScan::Span Root(const std::string& json);
JsonScan::Span Object(const std::string& json, const JsonScan::Span& parent, const char* key);
std::string Raw(const std::string& json, const JsonScan::Span& parent, const char* key);
std::wstring String(const std::string& raw);
}
