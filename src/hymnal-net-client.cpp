#include "hymnal-net-client.hpp"

#include <algorithm>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QTextDocument>
#include <QUrl>
#include <QSet>

namespace {

QString CleanHtmlFragment(const QString &fragment)
{
	QTextDocument doc;
	doc.setHtml(fragment);

	// hymnal.net indents continuation lines, sometimes with &nbsp; runs that
	// survive toPlainText() as U+00A0 and would then render as a ragged left
	// edge on the display source. Turn those into ordinary spaces so trimming
	// can reach them, and flush every line against the margin.
	QString text = doc.toPlainText();
	text.replace(QChar(0x00A0), QLatin1Char(' '));

	QStringList lines = text.split(QLatin1Char('\n'));
	for (QString &line : lines)
		line = line.trimmed();

	while (!lines.isEmpty() && lines.first().isEmpty())
		lines.removeFirst();
	while (!lines.isEmpty() && lines.last().isEmpty())
		lines.removeLast();

	return lines.join(QLatin1Char('\n'));
}

} // namespace

HymnalNetClient::HymnalNetClient(QObject *parent) : QObject(parent)
{
	nam = new QNetworkAccessManager(this);
}

void HymnalNetClient::Search(const QString &query)
{
	QUrl url(QStringLiteral("https://www.hymnal.net/en/search/all/all/") +
		 QString::fromUtf8(QUrl::toPercentEncoding(query)));

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OBS-Hymnal-Plugin/0.1"));

	QNetworkReply *reply = nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit SearchFailed(reply->errorString());
			return;
		}

		QString html = QString::fromUtf8(reply->readAll());

		QList<HymnalNetResult> results;
		QSet<QString> seenPaths;

		/*
		 * Search results are anchors like:
		 *   <a class="list-group-item" href="/en/hymn/h/813">
		 *     <span class="label label-info">E813</span> I come before Thy throne </a>
		 * so the href is not the first attribute and the label span sits
		 * inside the link text.
		 */
		QRegularExpression linkRe(QStringLiteral("<a[^>]*href=\"(/en/hymn/[a-zA-Z]+/\\w+)\"[^>]*>(.*?)</a>"),
					  QRegularExpression::DotMatchesEverythingOption);
		auto it = linkRe.globalMatch(html);
		while (it.hasNext()) {
			QRegularExpressionMatch m = it.next();
			QString path = m.captured(1);
			QString title = CleanHtmlFragment(m.captured(2)).simplified();
			if (title.isEmpty() || seenPaths.contains(path))
				continue;
			seenPaths.insert(path);
			results.push_back(HymnalNetResult{title, path});
		}

		if (results.isEmpty()) {
			emit SearchFailed(QStringLiteral("No results found."));
			return;
		}

		emit SearchFinished(results);
	});
}

void HymnalNetClient::FetchHymn(const QString &path)
{
	QUrl url(QStringLiteral("https://www.hymnal.net") + path);

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OBS-Hymnal-Plugin/0.1"));

	QNetworkReply *reply = nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply, path]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit FetchFailed(reply->errorString());
			return;
		}

		QString html = QString::fromUtf8(reply->readAll());

		QString title;
		QRegularExpression titleRe(QStringLiteral("<title>\\s*Hymn:\\s*(.*?)\\s*</title>"),
					   QRegularExpression::CaseInsensitiveOption);
		QRegularExpressionMatch titleMatch = titleRe.match(html);
		if (titleMatch.hasMatch())
			title = CleanHtmlFragment(titleMatch.captured(1));

		Hymn hymn;
		hymn.title = title.toStdString();

		QRegularExpression numberRe(QStringLiteral("/(\\d+)$"));
		QRegularExpressionMatch numberMatch = numberRe.match(path);
		if (numberMatch.hasMatch())
			hymn.number = numberMatch.captured(1).toInt();

		/*
		 * Each stanza is a <div data-type="verse|chorus" ...> block holding
		 * one <div class="text-container ..."> with the stanza text (chord
		 * data lives in a sibling chord-container). Segment the page by
		 * stanza start so each text is read from within its own stanza
		 * rather than pairing two independently collected lists, which
		 * silently misaligns when one stanza's markup differs.
		 */
		QRegularExpression stanzaRe(QStringLiteral("data-type=\"(verse|chorus)\""));
		QRegularExpression textRe(QStringLiteral("<div class=\"[^\"]*text-container[^\"]*\">(.*?)</div>"),
					  QRegularExpression::DotMatchesEverythingOption);

		QList<QPair<QString, qsizetype>> stanzaStarts;
		auto stanzaIt = stanzaRe.globalMatch(html);
		while (stanzaIt.hasNext()) {
			QRegularExpressionMatch m = stanzaIt.next();
			stanzaStarts.push_back({m.captured(1), m.capturedEnd(0)});
		}

		for (qsizetype i = 0; i < stanzaStarts.size(); ++i) {
			qsizetype begin = stanzaStarts[i].second;
			qsizetype end = (i + 1 < stanzaStarts.size()) ? stanzaStarts[i + 1].second : html.size();

			QRegularExpressionMatch textMatch = textRe.match(html, begin);
			if (!textMatch.hasMatch() || textMatch.capturedStart(0) >= end)
				continue;

			QString clean = CleanHtmlFragment(textMatch.captured(1));
			if (clean.isEmpty())
				continue;

			if (stanzaStarts[i].first == QStringLiteral("chorus")) {
				if (hymn.chorus.empty())
					hymn.chorus = clean.toStdString();
			} else {
				hymn.verses.push_back(clean.toStdString());
			}
		}

		if (hymn.title.empty() && hymn.verses.empty()) {
			emit FetchFailed(QStringLiteral("Could not parse hymn page."));
			return;
		}

		emit HymnFetched(hymn);
	});
}
