#pragma once

#include <string>
#include <vector>
#include <optional>

/*
Hymn JSON file format (one hymn per file), e.g. data/hymnal/001-amazing-grace.json:

{
  "number": 1,
  "title": "Amazing Grace",
  "author": "John Newton",
  "verses": [
    "Amazing grace! How sweet the sound\nThat saved a wretch like me...",
    "..."
  ],
  "chorus": "optional chorus text, omit or leave empty if the hymn has none"
}
*/

struct Hymn {
	int number = 0;
	std::string title;
	std::string author;
	std::vector<std::string> verses;
	std::string chorus;
	std::string filePath;

	// e.g. "23 - Amazing Grace"
	std::string DisplayName() const;
};

// Parses a single hymn JSON file. Returns std::nullopt on read/parse failure.
std::optional<Hymn> LoadHymnFile(const std::string &filePath);

// Loads every *.json file directly inside folderPath as a hymn, sorted by
// number (falling back to title for hymns sharing a number or numbered 0).
std::vector<Hymn> LoadHymnalFolder(const std::string &folderPath);

// Writes hymn as a new JSON file inside folderPath (creating folderPath if
// needed), deriving the filename from its number/title and disambiguating
// against any existing file of the same name. Returns the path written, or
// an empty string on failure.
std::string SaveHymnFile(const Hymn &hymn, const std::string &folderPath);
