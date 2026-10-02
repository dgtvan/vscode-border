#pragma once

#include <string>
#include <vector>

// Loads the last-saved manual project-list order: a list of alias keys
// (see label_alias.h) in display order. Returns an empty vector if manual
// ordering has never been saved.
std::vector<std::wstring> LoadItemOrder();

// Persists `order` (a list of alias keys, in display order) to
// project_list_order.ini next to config.ini, overwriting any previous save.
// Session keys (see label_alias.h's SessionAliasKey) are left out -- they
// mean nothing once their window closes.
void SaveItemOrder(const std::vector<std::wstring>& order);
