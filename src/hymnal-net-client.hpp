#pragma once

#include <QObject>
#include <QString>
#include <QList>

#include "hymn-data.hpp"

// A search result from hymnal.net: a title plus the site-relative path to
// its full hymn page (e.g. "/en/hymn/h/313").
struct HymnalNetResult {
	QString title;
	QString path;
};

// Minimal client for hymnal.net's own (undocumented but freely accessible,
// non-bot-blocked) search and hymn-page HTML, used to import a hymn's text
// into the local library. hymnal.net publishes full lyrics for all of its
// own hymns (it is the copyright holder / publisher for its catalog), so
// this only ever imports what the site already serves publicly.
class HymnalNetClient : public QObject {
	Q_OBJECT

public:
	explicit HymnalNetClient(QObject *parent = nullptr);

	// Searches hymnal.net for query. Emits exactly one of SearchFinished or
	// SearchFailed.
	void Search(const QString &query);

	// Fetches and parses the hymn page at path (as returned in a
	// HymnalNetResult). Emits exactly one of HymnFetched or FetchFailed.
	void FetchHymn(const QString &path);

signals:
	void SearchFinished(const QList<HymnalNetResult> &results);
	void SearchFailed(const QString &error);
	void HymnFetched(const Hymn &hymn);
	void FetchFailed(const QString &error);

private:
	class QNetworkAccessManager *nam = nullptr;
};
