#pragma once
#include "config.h"

std::wstring NormalizeProfilePath(const std::wstring& path);
std::wstring DefaultProviderPath(ProfileType type); // legacy migration / detection only
std::wstring ProfileId(const Profile& profile);
const wchar_t* ProfileTypeName(ProfileType type);
void ReadProfileConfig(const std::string& content, Config& config);

// Pure conversion, then an exact backup + atomic replacement wrapper.
// New-format files are never reinterpreted using legacy defaults.
bool MigrateLegacyProfiles(const std::string& content, std::string& converted);
std::string MigrateProfileConfigFile(const std::wstring& path, const std::string& content);
