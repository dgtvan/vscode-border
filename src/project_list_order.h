#pragma once

#include <string>
#include <vector>

// Loads the last-saved manual project-list order: a list of raw labels
// (see label_alias.h) in display order. Returns an empty vector if manual
// ordering has never been saved.
std::vector<std::wstring> LoadItemOrder();

// Persists `order` (a list of raw labels, in display order) to
// project_list_order.ini next to config.ini, overwriting any previous save.
void SaveItemOrder(const std::vector<std::wstring>& order);
