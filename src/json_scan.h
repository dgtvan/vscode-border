#pragma once

#include <string>
#include <vector>

// Minimal JSONC span-scanner: finds/inserts/removes specific keys (in an
// object) or elements (in an array) by locating their exact text spans,
// without ever parsing or reserializing anything else in the file. This is
// a generalization of vscode_settings.cpp's proven "manage one external
// settings.json key without disturbing the rest of the file" scanner --
// vscode_settings.cpp is intentionally left as its own copy (not migrated
// to share this) since it already works and touching it risks regressing a
// separate, already-delicate feature for a DRY nicety.
//
// All positions are byte offsets into the `content` string passed to each
// function. "Content span" for an object/array means the range *between*
// its braces/brackets (excluding them), e.g. for `{"a":1}` starting at
// index 5, the content span is [6, 12) -- the text `"a":1`.
namespace JsonScan {

struct Span {
    bool found = false;
    size_t start = 0;
    size_t end = 0; // exclusive
};

struct KeyEntry {
    bool found = false;
    size_t keyStart = 0;   // position of the key's opening quote
    size_t valueStart = 0; // start of the value (whitespace/comments already skipped)
    size_t valueEnd = 0;   // one past the last non-whitespace char of the value (includes a
                            // matching closing brace/bracket if the value is an object/array)
};

size_t SkipWhitespaceAndComments(const std::string& s, size_t i);

// Scans a JSON value starting at `start` (whitespace/comments already
// skipped by the caller). Returns the trimmed exclusive end of the value --
// stops at a comma or closing bracket that belongs to the *containing*
// structure, without consuming it. If the value is an object/array, the
// returned end is one past its own matching closing brace/bracket.
size_t FindValueEnd(const std::string& s, size_t start);

// Finds the first `{` not inside a string/comment -- the root object's open
// brace. Returns std::string::npos if none exists (empty/missing file).
size_t FindRootObjectOpenBrace(const std::string& s);

// Ensures `content` has a root JSON object, synthesizing an empty `{\n}\n`
// if the file is missing/empty/has no object at all (`content` is mutated
// in that case). Returns the object's content span.
Span EnsureRootObject(std::string& content);

// Finds `"key": <value>` directly inside the object whose content span is
// [objStart, objEnd) -- i.e. at depth 0 relative to that span, ignoring
// matches inside comments, strings, or further-nested objects/arrays.
KeyEntry FindKeyInObject(const std::string& content, size_t objStart, size_t objEnd, const std::string& key);

// Enumerates each top-level (depth 0 relative to the span), comma-separated
// element of an array's content span [arrStart, arrEnd), in order.
std::vector<Span> FindArrayElements(const std::string& content, size_t arrStart, size_t arrEnd);

// Removes a `"key": <value>` entry (from FindKeyInObject) from its object,
// plus whichever adjacent comma/whitespace keeps the object valid -- so the
// result looks like the entry was never there, not like a blank line was
// left behind.
std::string RemoveObjectKey(const std::string& content, const KeyEntry& entry);

// Same bookkeeping as RemoveObjectKey, for one array element (from
// FindArrayElements) instead of an object key.
std::string RemoveArrayElement(const std::string& content, const Span& element);

// Inserts `"key": <valueJson>` into the object whose content span is
// [objStart, objEnd). `valueJson` is inserted verbatim (caller is
// responsible for it being valid JSON).
std::string InsertIntoObject(std::string content, size_t objStart, size_t objEnd, const std::string& key,
                             const std::string& valueJson);

// Inserts `elementJson` as a new last element into the array whose content
// span is [arrStart, arrEnd).
std::string InsertIntoArray(std::string content, size_t arrStart, size_t arrEnd, const std::string& elementJson);

} // namespace JsonScan
