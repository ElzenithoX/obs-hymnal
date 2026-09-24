#include "hymnal-dock.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QGroupBox>
#include <QShowEvent>
#include <QDialog>
#include <QFormLayout>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QRegularExpression>
#include <QListWidgetItem>
#include <QCoreApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QAction>
#include <QScrollArea>
#include <QFrame>

#include "hymnal-net-client.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <nlohmann/json.hpp>

#include "plugin-support.h"

using json = nlohmann::json;

namespace {

std::string ConfigDir()
{
	char *path = obs_module_config_path("");
	std::string dir = path ? path : "";
	bfree(path);
	return dir;
}

std::string ConfigFilePath()
{
	char *path = obs_module_config_path("hymnal-dock.json");
	std::string p = path ? path : "";
	bfree(path);
	return p;
}

// Picks the folder a first-time user starts with and fills it from the sample
// hymns bundled in the plugin's data directory, so the dock has something to
// show the moment it is installed. The library lives in the config directory
// rather than the install directory so it stays writable, survives plugin
// updates, and is not lost when the plugin is uninstalled.
std::string CreateDefaultHymnalFolder()
{
	char *configPath = obs_module_config_path("hymnal");
	std::string target = configPath ? configPath : "";
	bfree(configPath);

	if (target.empty())
		return target;

	std::error_code ec;
	std::filesystem::create_directories(target, ec);
	if (ec) {
		obs_log(LOG_WARNING, "Hymnal: could not create '%s': %s", target.c_str(), ec.message().c_str());
		return std::string();
	}

	char *bundledPath = obs_module_file("hymnal");
	if (!bundledPath)
		return target;

	for (const auto &entry : std::filesystem::directory_iterator(bundledPath, ec)) {
		if (!entry.is_regular_file())
			continue;

		// Only seed real hymn files and the template. A build or editing
		// tool that leaves something else in the data folder should not
		// end up littering every new user's library with it.
		std::string extension = entry.path().extension().string();
		if (extension != ".json" && extension != ".example")
			continue;

		std::filesystem::path destination = std::filesystem::path(target) / entry.path().filename();
		std::error_code copyError;
		std::filesystem::copy_file(entry.path(), destination, std::filesystem::copy_options::skip_existing,
					   copyError);
		if (copyError)
			obs_log(LOG_WARNING, "Hymnal: could not copy sample hymn '%s': %s",
				entry.path().filename().string().c_str(), copyError.message().c_str());
	}

	bfree(bundledPath);
	return target;
}

bool CollectHymnalSourceNames(void *param, obs_source_t *source)
{
	auto *names = static_cast<QStringList *>(param);
	if (source && strcmp(obs_source_get_unversioned_id(source), "hymnal_text_source") == 0)
		names->append(QString::fromUtf8(obs_source_get_name(source)));
	return true;
}

// Merges `changes` into a source's settings and applies them in one update, so
// a verse push (text + reference label) costs the text source a single
// re-rasterisation rather than one per key.
void ApplyChangesToSource(obs_source_t *source, obs_data_t *changes)
{
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_apply(settings, changes);
	obs_source_update(source, settings);
	obs_data_release(settings);
}

bool ApplyChangesOnAllHymnalSources(void *param, obs_source_t *source)
{
	if (source && strcmp(obs_source_get_unversioned_id(source), "hymnal_text_source") == 0)
		ApplyChangesToSource(source, static_cast<obs_data_t *>(param));
	return true;
}

// Hymn text carries the indentation of wherever it came from — hymnal.net
// indents every second line, with plain spaces on some pages and non-breaking
// ones on others — which renders as a ragged left edge whatever justification
// the display source is set to. Flush every line against the margin and drop
// blank leading/trailing lines so the source's align setting is the only thing
// deciding horizontal placement.
QString FlushLines(const QString &text)
{
	QString normalized = text;
	normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
	normalized.replace(QChar(0x00A0), QLatin1Char(' '));
	normalized.replace(QLatin1Char('\t'), QLatin1Char(' '));

	QStringList lines = normalized.split(QLatin1Char('\n'));
	for (QString &line : lines)
		line = line.trimmed();

	while (!lines.isEmpty() && lines.first().isEmpty())
		lines.removeFirst();
	while (!lines.isEmpty() && lines.last().isEmpty())
		lines.removeLast();

	return lines.join(QLatin1Char('\n'));
}

std::string FlushLines(const std::string &text)
{
	return FlushLines(QString::fromStdString(text)).toStdString();
}

// Splits text into verses on blank-line boundaries (one or more empty lines),
// trimming each verse and dropping empty ones.
std::vector<std::string> ParseVerses(const QString &text)
{
	std::vector<std::string> verses;

	QString normalized = text;
	normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

	for (const QString &block : normalized.split(QRegularExpression(QStringLiteral("\\n\\s*\\n")))) {
		QString flushed = FlushLines(block);
		if (!flushed.isEmpty())
			verses.push_back(flushed.toStdString());
	}

	return verses;
}

// Shows a modal form pre-filled from hymn, looping until the user provides a
// valid title + at least one verse or cancels. Returns true and updates hymn
// in place if accepted, false if cancelled. Does not touch disk.
bool EditHymnDialog(QWidget *parent, const QString &windowTitle, Hymn &hymn)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(windowTitle);

	auto *layout = new QFormLayout(&dialog);

	auto *numberSpin = new QSpinBox(&dialog);
	numberSpin->setRange(0, 9999);
	numberSpin->setSpecialValueText(QStringLiteral("(none)"));
	numberSpin->setValue(hymn.number);
	layout->addRow(QStringLiteral("Number:"), numberSpin);

	auto *titleEdit = new QLineEdit(QString::fromStdString(hymn.title), &dialog);
	layout->addRow(QStringLiteral("Title:"), titleEdit);

	auto *authorEdit = new QLineEdit(QString::fromStdString(hymn.author), &dialog);
	layout->addRow(QStringLiteral("Author:"), authorEdit);

	auto *versesEdit = new QPlainTextEdit(&dialog);
	versesEdit->setPlaceholderText(
		QStringLiteral("Verse 1 text...\n\nVerse 2 text...\n\n(separate verses with a blank line)"));
	versesEdit->setMinimumHeight(160);
	QStringList verseBlocks;
	for (const auto &v : hymn.verses)
		verseBlocks << QString::fromStdString(v);
	versesEdit->setPlainText(verseBlocks.join(QStringLiteral("\n\n")));
	layout->addRow(QStringLiteral("Verses:"), versesEdit);

	auto *chorusEdit = new QPlainTextEdit(QString::fromStdString(hymn.chorus), &dialog);
	chorusEdit->setPlaceholderText(QStringLiteral("Optional chorus/refrain"));
	chorusEdit->setMaximumHeight(80);
	layout->addRow(QStringLiteral("Chorus:"), chorusEdit);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addRow(buttons);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	while (true) {
		if (dialog.exec() != QDialog::Accepted)
			return false;

		QString title = titleEdit->text().trimmed();
		std::vector<std::string> verses = ParseVerses(versesEdit->toPlainText());

		if (title.isEmpty() || verses.empty()) {
			QMessageBox::warning(&dialog, windowTitle,
					     QStringLiteral("A title and at least one verse are required."));
			continue;
		}

		hymn.number = numberSpin->value();
		hymn.title = title.toStdString();
		hymn.author = authorEdit->text().trimmed().toStdString();
		hymn.verses = verses;
		hymn.chorus = chorusEdit->toPlainText().trimmed().toStdString();
		return true;
	}
}

} // namespace

HymnalDock::HymnalDock(QWidget *parent) : QWidget(parent)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	// Shown only while the dock is floating: a one-click way back into the
	// main window for anyone who misses the drag target.
	dockHintRow = new QWidget(this);
	auto *dockHintLayout = new QHBoxLayout(dockHintRow);
	dockHintLayout->setContentsMargins(6, 6, 6, 0);
	auto *dockHint =
		new QLabel(QStringLiteral("Floating — drag the title bar onto an edge of OBS, or"), dockHintRow);
	dockHint->setWordWrap(true);
	dockNowButton = new QPushButton(QStringLiteral("Dock into OBS"), dockHintRow);
	dockHintLayout->addWidget(dockHint, 1);
	dockHintLayout->addWidget(dockNowButton);
	dockHintRow->hide();
	outer->addWidget(dockHintRow);
	connect(dockNowButton, &QPushButton::clicked, this, &HymnalDock::OnDockNow);

	// Everything else lives inside a scroll area. QMainWindow refuses to
	// dock a dragged widget when the layout's minimum size would no longer
	// fit the window, and this dock's natural minimum (three button rows, a
	// list, a preview) is tall enough to trip that on a 1080p layout with
	// Studio Mode on. A scroll area's minimum is a few dozen pixels, so any
	// dock area will take it, and the content just scrolls if squeezed.
	auto *content = new QWidget(this);
	auto *scroll = new QScrollArea(this);
	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	outer->addWidget(scroll, 1);

	auto *root = new QVBoxLayout(content);

	// Hymnal folder row
	auto *folderRow = new QHBoxLayout();
	folderEdit = new QLineEdit(this);
	folderEdit->setReadOnly(true);
	folderEdit->setPlaceholderText(QStringLiteral("No hymnal folder selected"));
	browseButton = new QPushButton(QStringLiteral("Browse..."), this);
	folderRow->addWidget(folderEdit, 1);
	folderRow->addWidget(browseButton);
	root->addLayout(folderRow);

	// Search + add
	auto *searchRow = new QHBoxLayout();
	searchEdit = new QLineEdit(this);
	searchEdit->setPlaceholderText(QStringLiteral("Search hymns..."));
	addHymnButton = new QPushButton(QStringLiteral("Add Hymn..."), this);
	searchOnlineButton = new QPushButton(QStringLiteral("Search Online..."), this);
	searchRow->addWidget(searchEdit, 1);
	searchRow->addWidget(addHymnButton);
	searchRow->addWidget(searchOnlineButton);
	root->addLayout(searchRow);

	hymnList = new QListWidget(this);
	root->addWidget(hymnList, 1);

	// Now showing / verse controls
	auto *verseGroup = new QGroupBox(QStringLiteral("Now Showing"), this);
	auto *verseLayout = new QVBoxLayout(verseGroup);

	verseLabel = new QLabel(QStringLiteral("No hymn selected"), verseGroup);
	verseLayout->addWidget(verseLabel);

	previewText = new QPlainTextEdit(verseGroup);
	previewText->setReadOnly(true);
	previewText->setMaximumHeight(110);
	verseLayout->addWidget(previewText);

	auto *navRow = new QHBoxLayout();
	prevButton = new QPushButton(QStringLiteral("◀ Prev"), verseGroup);
	nextButton = new QPushButton(QStringLiteral("Next ▶"), verseGroup);
	chorusButton = new QPushButton(QStringLiteral("Chorus"), verseGroup);
	clearButton = new QPushButton(QStringLiteral("Clear"), verseGroup);
	navRow->addWidget(prevButton);
	navRow->addWidget(nextButton);
	navRow->addWidget(chorusButton);
	navRow->addWidget(clearButton);
	verseLayout->addLayout(navRow);

	root->addWidget(verseGroup);

	// Target source rows
	auto *targetGroup = new QGroupBox(QStringLiteral("Send To"), this);
	auto *targetLayout = new QVBoxLayout(targetGroup);

	auto *targetRow = new QHBoxLayout();
	targetCombo = new QComboBox(targetGroup);
	refreshTargetsButton = new QPushButton(QStringLiteral("Refresh"), targetGroup);
	targetRow->addWidget(targetCombo, 1);
	targetRow->addWidget(refreshTargetsButton);
	targetLayout->addLayout(targetRow);

	auto *alignRow = new QHBoxLayout();
	alignRow->addWidget(new QLabel(QStringLiteral("Justify:"), targetGroup));
	auto *alignLeftButton = new QPushButton(QStringLiteral("Left"), targetGroup);
	auto *alignCenterButton = new QPushButton(QStringLiteral("Center"), targetGroup);
	auto *alignRightButton = new QPushButton(QStringLiteral("Right"), targetGroup);
	alignRow->addWidget(alignLeftButton);
	alignRow->addWidget(alignCenterButton);
	alignRow->addWidget(alignRightButton);
	targetLayout->addLayout(alignRow);

	connect(alignLeftButton, &QPushButton::clicked, this, [this]() { ApplySettingToTargets("align", "left"); });
	connect(alignCenterButton, &QPushButton::clicked, this, [this]() { ApplySettingToTargets("align", "center"); });
	connect(alignRightButton, &QPushButton::clicked, this, [this]() { ApplySettingToTargets("align", "right"); });

	root->addWidget(targetGroup);

	connect(browseButton, &QPushButton::clicked, this, &HymnalDock::OnBrowseFolder);
	connect(searchEdit, &QLineEdit::textChanged, this, &HymnalDock::OnSearchTextChanged);
	connect(hymnList, &QListWidget::currentRowChanged, this, &HymnalDock::OnHymnRowChanged);
	connect(prevButton, &QPushButton::clicked, this, &HymnalDock::OnPrevVerse);
	connect(nextButton, &QPushButton::clicked, this, &HymnalDock::OnNextVerse);
	connect(chorusButton, &QPushButton::clicked, this, &HymnalDock::OnShowChorus);
	connect(clearButton, &QPushButton::clicked, this, &HymnalDock::OnClearDisplay);
	connect(refreshTargetsButton, &QPushButton::clicked, this, &HymnalDock::OnRefreshTargets);
	connect(addHymnButton, &QPushButton::clicked, this, &HymnalDock::OnAddHymn);
	connect(searchOnlineButton, &QPushButton::clicked, this, &HymnalDock::OnSearchOnline);

	LoadConfig();
	if (hymnalFolder.empty()) {
		hymnalFolder = CreateDefaultHymnalFolder();
		if (!hymnalFolder.empty())
			SaveConfig();
	}
	if (!hymnalFolder.empty()) {
		folderEdit->setText(QString::fromStdString(hymnalFolder));
		ReloadHymnal();
	}
	RefreshTargetSources();
	UpdateVerseControls();
}

HymnalDock::~HymnalDock() {}

QDockWidget *HymnalDock::FindDockWidget() const
{
	for (QWidget *widget = parentWidget(); widget; widget = widget->parentWidget()) {
		if (auto *dock = qobject_cast<QDockWidget *>(widget))
			return dock;
	}
	return nullptr;
}

QMainWindow *HymnalDock::MainWindow() const
{
	return qobject_cast<QMainWindow *>(static_cast<QWidget *>(obs_frontend_get_main_window()));
}

bool HymnalDock::DockIntoMainWindow()
{
	QDockWidget *dockWidget = FindDockWidget();
	QMainWindow *mainWindow = MainWindow();
	if (!dockWidget || !mainWindow)
		return false;

	mainWindow->addDockWidget(Qt::RightDockWidgetArea, dockWidget);
	dockWidget->setFloating(false);
	dockWidget->setVisible(true);
	dockWidget->raise();

	return mainWindow->dockWidgetArea(dockWidget) != Qt::NoDockWidgetArea;
}

void HymnalDock::OnDockNow()
{
	DockIntoMainWindow();
}

void HymnalDock::EnsureDockable()
{
	QDockWidget *dockWidget = FindDockWidget();
	QMainWindow *mainWindow = MainWindow();
	if (!dockWidget || !mainWindow)
		return;

	// A dock Qt will not accept into any area, or that it refuses to let the
	// user drag, reads as "not dockable" even though it is registered — so
	// state both explicitly rather than relying on whatever OBS left behind.
	// Docks/Lock UI still wins: OBS clears the features of every plugin dock
	// while it is on, and re-applies them when toggled.
	auto *lockDocks = mainWindow->findChild<QAction *>(QStringLiteral("lockDocks"));
	bool locked = lockDocks && lockDocks->isChecked();

	dockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
	if (!locked)
		dockWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
					QDockWidget::DockWidgetFloatable);

	if (!dockSignalsConnected) {
		dockSignalsConnected = true;
		connect(dockWidget, &QDockWidget::topLevelChanged, this,
			[this](bool floating) { dockHintRow->setVisible(floating); });
	}
	dockHintRow->setVisible(dockWidget->isFloating());

	// Qt only offers a drop target while the dock's minimum still fits the
	// window, so leave the numbers in the log for when a drag "does nothing".
	QSize dockMin = dockWidget->minimumSizeHint();
	obs_log(LOG_INFO, "Hymnal dock: floating=%d area=%d features=0x%x min=%dx%d window=%dx%d",
		dockWidget->isFloating() ? 1 : 0, (int)mainWindow->dockWidgetArea(dockWidget),
		(unsigned)dockWidget->features(), dockMin.width(), dockMin.height(), mainWindow->width(),
		mainWindow->height());

	if (dockedOnce)
		return;

	// Only remember the embed once it actually took: otherwise a run where
	// the main window was not ready would leave the dock floating forever.
	if (!DockIntoMainWindow())
		return;

	dockedOnce = true;
	SaveConfig();
}

void HymnalDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	RefreshTargetSources();

	if (QDockWidget *dockWidget = FindDockWidget())
		dockHintRow->setVisible(dockWidget->isFloating());
}

QSize HymnalDock::sizeHint() const
{
	// What the dock asks for when first dropped into an area; the scroll
	// area underneath lets it be squeezed well below this.
	return QSize(420, 640);
}

void HymnalDock::LoadConfig()
{
	std::ifstream file(ConfigFilePath(), std::ios::binary);
	if (!file.is_open())
		return;

	std::stringstream buffer;
	buffer << file.rdbuf();

	try {
		json root = json::parse(buffer.str());
		hymnalFolder = root.value("folder", std::string());
		dockedOnce = root.value("dockedOnce", false);
	} catch (const json::parse_error &) {
		hymnalFolder.clear();
	}
}

void HymnalDock::SaveConfig()
{
	os_mkdirs(ConfigDir().c_str());

	json root;
	root["folder"] = hymnalFolder;
	root["dockedOnce"] = dockedOnce;

	std::ofstream file(ConfigFilePath(), std::ios::binary | std::ios::trunc);
	if (!file.is_open()) {
		obs_log(LOG_WARNING, "Hymnal: could not write dock config to '%s'", ConfigFilePath().c_str());
		return;
	}
	file << root.dump(2);
}

void HymnalDock::OnBrowseFolder()
{
	QWidget *mainWindow = reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
	QString dir = QFileDialog::getExistingDirectory(mainWindow ? mainWindow : this,
							QStringLiteral("Select Hymnal Folder"),
							QString::fromStdString(hymnalFolder));
	if (dir.isEmpty())
		return;

	hymnalFolder = dir.toStdString();
	folderEdit->setText(dir);
	ReloadHymnal();
	SaveConfig();
}

void HymnalDock::ReloadHymnal()
{
	hymns = LoadHymnalFolder(hymnalFolder);
	currentHymnIndex = -1;
	currentVerseIndex = -1;
	showingChorus = false;
	previewText->clear();
	ApplyFilter();
	UpdateVerseControls();
}

void HymnalDock::OnSearchTextChanged(const QString &)
{
	ApplyFilter();
}

void HymnalDock::ApplyFilter()
{
	QString filter = searchEdit->text().trimmed().toLower();

	filteredIndices.clear();
	hymnList->blockSignals(true);
	hymnList->clear();

	for (size_t i = 0; i < hymns.size(); ++i) {
		const Hymn &hymn = hymns[i];

		bool matches = filter.isEmpty();
		if (!matches) {
			QString title = QString::fromStdString(hymn.title).toLower();
			QString author = QString::fromStdString(hymn.author).toLower();
			QString number = QString::number(hymn.number);
			matches = title.contains(filter) || author.contains(filter) || number.contains(filter);
		}

		if (matches) {
			filteredIndices.push_back(static_cast<int>(i));
			hymnList->addItem(QString::fromStdString(hymn.DisplayName()));
		}
	}

	hymnList->blockSignals(false);

	// Re-select the currently active hymn in the filtered list if present.
	if (currentHymnIndex >= 0) {
		for (size_t row = 0; row < filteredIndices.size(); ++row) {
			if (filteredIndices[row] == currentHymnIndex) {
				hymnList->setCurrentRow(static_cast<int>(row));
				return;
			}
		}
	}
}

void HymnalDock::OnHymnRowChanged(int row)
{
	if (row < 0 || row >= static_cast<int>(filteredIndices.size()))
		return;

	SelectHymn(filteredIndices[row]);
}

void HymnalDock::SelectHymn(int hymnIndex)
{
	if (hymnIndex < 0 || hymnIndex >= static_cast<int>(hymns.size()))
		return;

	currentHymnIndex = hymnIndex;
	showingChorus = false;
	currentVerseIndex = hymns[hymnIndex].verses.empty() ? -1 : 0;

	PushCurrentSegment();
	UpdateVerseControls();
}

void HymnalDock::OnPrevVerse()
{
	if (currentHymnIndex < 0)
		return;

	showingChorus = false;
	if (currentVerseIndex > 0)
		currentVerseIndex--;

	PushCurrentSegment();
	UpdateVerseControls();
}

void HymnalDock::OnNextVerse()
{
	if (currentHymnIndex < 0)
		return;

	const Hymn &hymn = hymns[currentHymnIndex];
	showingChorus = false;
	if (currentVerseIndex + 1 < static_cast<int>(hymn.verses.size()))
		currentVerseIndex++;

	PushCurrentSegment();
	UpdateVerseControls();
}

void HymnalDock::OnShowChorus()
{
	if (currentHymnIndex < 0)
		return;

	const Hymn &hymn = hymns[currentHymnIndex];
	if (hymn.chorus.empty())
		return;

	showingChorus = true;
	PushCurrentSegment();
	UpdateVerseControls();
}

void HymnalDock::OnClearDisplay()
{
	previewText->clear();
	PushTextToTargets("");
}

void HymnalDock::PushCurrentSegment()
{
	if (currentHymnIndex < 0)
		return;

	const Hymn &hymn = hymns[currentHymnIndex];
	std::string text;

	if (showingChorus) {
		text = hymn.chorus;
	} else if (currentVerseIndex >= 0 && currentVerseIndex < static_cast<int>(hymn.verses.size())) {
		text = hymn.verses[currentVerseIndex];
	}

	text = FlushLines(text);

	previewText->setPlainText(QString::fromStdString(text));

	// The overlay's reference box reads like a scripture citation — which
	// hymn on the first line, which part of it on the second.
	std::string label = "HYMN";
	if (hymn.number > 0)
		label += " " + std::to_string(hymn.number);
	label += showingChorus ? "\nCHORUS" : "\nVERSE " + std::to_string(currentVerseIndex + 1);

	PushTextToTargets(text, label);
}

void HymnalDock::UpdateVerseControls()
{
	bool hasHymn = currentHymnIndex >= 0 && currentHymnIndex < static_cast<int>(hymns.size());
	const Hymn *hymn = hasHymn ? &hymns[currentHymnIndex] : nullptr;

	prevButton->setEnabled(hasHymn && !showingChorus && currentVerseIndex > 0);
	nextButton->setEnabled(hasHymn && hymn && !hymn->verses.empty() &&
			       (showingChorus || currentVerseIndex + 1 < static_cast<int>(hymn->verses.size())));
	chorusButton->setEnabled(hasHymn && hymn && !hymn->chorus.empty());

	if (!hasHymn) {
		verseLabel->setText(QStringLiteral("No hymn selected"));
		return;
	}

	if (showingChorus) {
		verseLabel->setText(QStringLiteral("%1 — Chorus").arg(QString::fromStdString(hymn->title)));
	} else if (currentVerseIndex >= 0 && !hymn->verses.empty()) {
		verseLabel->setText(QStringLiteral("%1 — Verse %2 / %3")
					    .arg(QString::fromStdString(hymn->title))
					    .arg(currentVerseIndex + 1)
					    .arg(static_cast<int>(hymn->verses.size())));
	} else {
		verseLabel->setText(QString::fromStdString(hymn->title) + QStringLiteral(" — (no verses)"));
	}
}

void HymnalDock::OnRefreshTargets()
{
	RefreshTargetSources();
}

void HymnalDock::RefreshTargetSources()
{
	QString previouslySelected = targetCombo->currentData().toString();
	bool hadSelection = targetCombo->count() > 0;

	targetCombo->blockSignals(true);
	targetCombo->clear();
	targetCombo->addItem(QStringLiteral("All Hymnal Display Sources"), QString());

	QStringList names;
	obs_enum_sources(&CollectHymnalSourceNames, &names);
	names.sort(Qt::CaseInsensitive);
	for (const QString &name : names)
		targetCombo->addItem(name, name);

	if (hadSelection) {
		int idx = targetCombo->findData(previouslySelected);
		targetCombo->setCurrentIndex(idx >= 0 ? idx : 0);
	}
	targetCombo->blockSignals(false);
}

void HymnalDock::ApplyChangesToTargets(obs_data_t *changes)
{
	QString selected = targetCombo->currentData().toString();

	if (selected.isEmpty()) {
		obs_enum_sources(&ApplyChangesOnAllHymnalSources, changes);
		return;
	}

	obs_source_t *src = obs_get_source_by_name(selected.toUtf8().constData());
	if (!src)
		return;

	ApplyChangesToSource(src, changes);
	obs_source_release(src);
}

void HymnalDock::ApplySettingToTargets(const char *key, const char *value)
{
	obs_data_t *changes = obs_data_create();
	obs_data_set_string(changes, key, value);
	ApplyChangesToTargets(changes);
	obs_data_release(changes);
}

void HymnalDock::PushTextToTargets(const std::string &text, const std::string &label)
{
	obs_data_t *changes = obs_data_create();
	obs_data_set_string(changes, "text", text.c_str());
	if (!label.empty())
		obs_data_set_string(changes, "overlay_label", label.c_str());
	ApplyChangesToTargets(changes);
	obs_data_release(changes);
}

void HymnalDock::SaveImportedHymn(const Hymn &hymn)
{
	std::string savedPath = SaveHymnFile(hymn, hymnalFolder);
	if (savedPath.empty()) {
		QMessageBox::warning(this, QStringLiteral("Hymnal"),
				     QStringLiteral("Could not save the hymn file. Check the log for details."));
		return;
	}

	ReloadHymnal();

	for (size_t i = 0; i < hymns.size(); ++i) {
		if (hymns[i].filePath != savedPath)
			continue;

		for (size_t row = 0; row < filteredIndices.size(); ++row) {
			if (filteredIndices[row] == static_cast<int>(i)) {
				hymnList->setCurrentRow(static_cast<int>(row));
				break;
			}
		}
		break;
	}
}

void HymnalDock::OnAddHymn()
{
	if (hymnalFolder.empty()) {
		QMessageBox::information(this, QStringLiteral("Add Hymn"),
					 QStringLiteral("Choose a hymnal folder first (Browse... button above)."));
		return;
	}

	Hymn hymn;
	if (EditHymnDialog(this, QStringLiteral("Add Hymn"), hymn))
		SaveImportedHymn(hymn);
}

void HymnalDock::OnSearchOnline()
{
	if (hymnalFolder.empty()) {
		QMessageBox::information(this, QStringLiteral("Search Online"),
					 QStringLiteral("Choose a hymnal folder first (Browse... button above)."));
		return;
	}

	QDialog dialog(this);
	dialog.setWindowTitle(QStringLiteral("Search hymnal.net"));
	dialog.resize(420, 480);

	auto *layout = new QVBoxLayout(&dialog);

	auto *searchRow = new QHBoxLayout();
	auto *queryEdit = new QLineEdit(&dialog);
	queryEdit->setPlaceholderText(QStringLiteral("Search hymnal.net..."));
	auto *searchButton = new QPushButton(QStringLiteral("Search"), &dialog);
	searchRow->addWidget(queryEdit, 1);
	searchRow->addWidget(searchButton);
	layout->addLayout(searchRow);

	auto *statusLabel = new QLabel(QStringLiteral("Search hymnal.net's published hymn catalog."), &dialog);
	statusLabel->setWordWrap(true);
	layout->addWidget(statusLabel);

	auto *resultsList = new QListWidget(&dialog);
	layout->addWidget(resultsList, 1);

	auto *importButton = new QPushButton(QStringLiteral("Import Selected..."), &dialog);
	importButton->setEnabled(false);
	layout->addWidget(importButton);

	auto *closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	layout->addWidget(closeButtons);
	connect(closeButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	auto *client = new HymnalNetClient(&dialog);

	auto runSearch = [&]() {
		QString query = queryEdit->text().trimmed();
		if (query.isEmpty())
			return;
		resultsList->clear();
		importButton->setEnabled(false);
		statusLabel->setText(QStringLiteral("Searching..."));
		searchButton->setEnabled(false);
		client->Search(query);
	};

	connect(searchButton, &QPushButton::clicked, &dialog, runSearch);
	connect(queryEdit, &QLineEdit::returnPressed, &dialog, runSearch);

	connect(client, &HymnalNetClient::SearchFinished, &dialog, [=](const QList<HymnalNetResult> &results) {
		searchButton->setEnabled(true);
		statusLabel->setText(QStringLiteral("%1 result(s). Select one, then Import.").arg(results.size()));
		for (const auto &r : results) {
			auto *item = new QListWidgetItem(r.title, resultsList);
			item->setData(Qt::UserRole, r.path);
		}
	});

	connect(client, &HymnalNetClient::SearchFailed, &dialog, [=](const QString &error) {
		searchButton->setEnabled(true);
		statusLabel->setText(error);
	});

	connect(resultsList, &QListWidget::currentRowChanged, &dialog,
		[importButton](int row) { importButton->setEnabled(row >= 0); });

	auto runImport = [&]() {
		QListWidgetItem *item = resultsList->currentItem();
		if (!item)
			return;
		importButton->setEnabled(false);
		searchButton->setEnabled(false);
		statusLabel->setText(QStringLiteral("Fetching hymn..."));
		client->FetchHymn(item->data(Qt::UserRole).toString());
	};
	connect(importButton, &QPushButton::clicked, &dialog, runImport);

	connect(client, &HymnalNetClient::HymnFetched, &dialog, [&](const Hymn &fetched) {
		Hymn hymn = fetched;
		dialog.accept();
		if (EditHymnDialog(this, QStringLiteral("Import Hymn"), hymn))
			SaveImportedHymn(hymn);
	});

	connect(client, &HymnalNetClient::FetchFailed, &dialog, [=](const QString &error) {
		importButton->setEnabled(true);
		searchButton->setEnabled(true);
		statusLabel->setText(error);
	});

	dialog.exec();
}

static void AddBundledQtPluginPath()
{
	// OBS's own Qt deployment does not ship the "tls" plugin category (it
	// has no need for QNetworkAccessManager HTTPS requests), so
	// QNetworkAccessManager fails with "TLS initialization failed" unless
	// we point Qt at our own bundled copy of the SChannel TLS backend.
	char *pluginDir = obs_module_file("qtplugins");
	if (pluginDir) {
		QCoreApplication::addLibraryPath(QString::fromUtf8(pluginDir));
		bfree(pluginDir);
	}
}

static void OnFrontendEvent(enum obs_frontend_event event, void *data)
{
	// Wait for FINISHED_LOADING so the embed happens after OBS has restored
	// the saved window layout (which would otherwise override it).
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	static_cast<HymnalDock *>(data)->EnsureDockable();
	obs_frontend_remove_event_callback(OnFrontendEvent, data);
}

void RegisterHymnalDock()
{
	AddBundledQtPluginPath();

	obs_frontend_push_ui_translation(obs_module_get_string);
	QString title = QString::fromUtf8(obs_module_text("HymnalDock.Title"));
	obs_frontend_pop_ui_translation();

	HymnalDock *dock = new HymnalDock();
	obs_frontend_add_dock_by_id("hymnal_browser_dock", title.toUtf8().constData(), dock);
	obs_frontend_add_event_callback(OnFrontendEvent, dock);
}
