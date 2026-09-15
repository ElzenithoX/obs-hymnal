#include "hymn-data.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <obs-module.h>

#include "plugin-support.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string SlugifyTitle(const std::string &title)
{
	std::string slug;
	slug.reserve(title.size());

	bool lastWasDash = false;
	for (unsigned char c : title) {
		if (std::isalnum(c)) {
			slug += static_cast<char>(std::tolower(c));
			lastWasDash = false;
		} else if (!lastWasDash && !slug.empty()) {
			slug += '-';
			lastWasDash = true;
		}
	}

	while (!slug.empty() && slug.back() == '-')
		slug.pop_back();

	return slug.empty() ? std::string("hymn") : slug;
}

} // namespace

std::string Hymn::DisplayName() const
{
	std::string name;
	if (number > 0)
		name = std::to_string(number) + " - ";
	name += title.empty() ? std::string("Untitled Hymn") : title;
	return name;
}

std::optional<Hymn> LoadHymnFile(const std::string &filePath)
{
	std::ifstream file(filePath, std::ios::binary);
	if (!file.is_open()) {
		obs_log(LOG_WARNING, "Hymnal: could not open '%s'", filePath.c_str());
		return std::nullopt;
	}

	std::stringstream buffer;
	buffer << file.rdbuf();

	json root;
	try {
		root = json::parse(buffer.str());
	} catch (const json::parse_error &e) {
		obs_log(LOG_WARNING, "Hymnal: failed to parse '%s': %s", filePath.c_str(), e.what());
		return std::nullopt;
	}

	if (!root.is_object()) {
		obs_log(LOG_WARNING, "Hymnal: '%s' does not contain a JSON object", filePath.c_str());
		return std::nullopt;
	}

	Hymn hymn;
	hymn.filePath = filePath;
	hymn.number = root.value("number", 0);
	hymn.title = root.value("title", std::string());
	hymn.author = root.value("author", std::string());
	hymn.chorus = root.value("chorus", std::string());

	if (root.contains("verses") && root["verses"].is_array()) {
		for (const auto &verse : root["verses"]) {
			if (verse.is_string())
				hymn.verses.push_back(verse.get<std::string>());
		}
	}

	if (hymn.title.empty() && hymn.verses.empty()) {
		obs_log(LOG_WARNING, "Hymnal: '%s' has no title and no verses, skipping", filePath.c_str());
		return std::nullopt;
	}

	return hymn;
}

std::vector<Hymn> LoadHymnalFolder(const std::string &folderPath)
{
	std::vector<Hymn> hymns;

	std::error_code ec;
	if (folderPath.empty() || !fs::is_directory(folderPath, ec))
		return hymns;

	for (const auto &entry : fs::directory_iterator(folderPath, ec)) {
		if (!entry.is_regular_file())
			continue;

		if (entry.path().extension() != ".json")
			continue;

		if (auto hymn = LoadHymnFile(entry.path().string()))
			hymns.push_back(std::move(*hymn));
	}

	std::sort(hymns.begin(), hymns.end(), [](const Hymn &a, const Hymn &b) {
		if (a.number != b.number)
			return a.number < b.number;
		return a.title < b.title;
	});

	return hymns;
}

std::string SaveHymnFile(const Hymn &hymn, const std::string &folderPath)
{
	std::error_code ec;
	fs::create_directories(folderPath, ec);

	std::string base = SlugifyTitle(hymn.title);
	if (hymn.number > 0) {
		char prefix[16];
		snprintf(prefix, sizeof(prefix), "%03d-", hymn.number);
		base = prefix + base;
	}

	fs::path target = fs::path(folderPath) / (base + ".json");
	for (int suffix = 1; fs::exists(target); ++suffix)
		target = fs::path(folderPath) / (base + "-" + std::to_string(suffix) + ".json");

	json root;
	root["number"] = hymn.number;
	root["title"] = hymn.title;
	root["author"] = hymn.author;
	root["verses"] = hymn.verses;
	root["chorus"] = hymn.chorus;

	std::ofstream file(target, std::ios::binary | std::ios::trunc);
	if (!file.is_open()) {
		obs_log(LOG_WARNING, "Hymnal: could not write hymn file '%s'", target.string().c_str());
		return std::string();
	}

	file << root.dump(2);
	return target.string();
}
