#pragma once

#include <QWidget>
#include <QString>

#include <string>
#include <vector>

#include "hymn-data.hpp"

class QLineEdit;
class QListWidget;
class QComboBox;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QShowEvent;
class QDockWidget;
class QMainWindow;
typedef struct obs_data obs_data_t;

class HymnalDock : public QWidget {
	Q_OBJECT

public:
	explicit HymnalDock(QWidget *parent = nullptr);
	~HymnalDock() override;

	// Makes sure Qt will let this dock be dragged into the main window, and
	// embeds it into a dock area the first time the plugin ever runs (new
	// docks otherwise start floating, and dragging a floating dock into the
	// layout is easy to miss). The embed only happens once, so the user's
	// saved layout wins afterwards.
	void EnsureDockable();

	QSize sizeHint() const override;

protected:
	void showEvent(QShowEvent *event) override;

private slots:
	void OnBrowseFolder();
	void OnSearchTextChanged(const QString &text);
	void OnHymnRowChanged(int row);
	void OnPrevVerse();
	void OnNextVerse();
	void OnShowChorus();
	void OnClearDisplay();
	void OnRefreshTargets();
	void OnAddHymn();
	void OnSearchOnline();
	void OnDockNow();

private:
	void LoadConfig();
	void SaveConfig();
	void ReloadHymnal();
	void ApplyFilter();
	void RefreshTargetSources();
	void SelectHymn(int hymnIndex);
	void PushCurrentSegment();
	void UpdateVerseControls();
	void PushTextToTargets(const std::string &text, const std::string &label = std::string());
	void ApplySettingToTargets(const char *key, const char *value);
	void ApplyChangesToTargets(obs_data_t *changes);
	void SaveImportedHymn(const Hymn &hymn);

	QDockWidget *FindDockWidget() const;
	QMainWindow *MainWindow() const;
	bool DockIntoMainWindow();

	QWidget *dockHintRow = nullptr;
	QPushButton *dockNowButton = nullptr;
	QLineEdit *folderEdit = nullptr;
	QPushButton *browseButton = nullptr;
	QPushButton *addHymnButton = nullptr;
	QPushButton *searchOnlineButton = nullptr;
	QLineEdit *searchEdit = nullptr;
	QListWidget *hymnList = nullptr;
	QComboBox *targetCombo = nullptr;
	QPushButton *refreshTargetsButton = nullptr;
	QPlainTextEdit *previewText = nullptr;
	QLabel *verseLabel = nullptr;
	QPushButton *prevButton = nullptr;
	QPushButton *nextButton = nullptr;
	QPushButton *chorusButton = nullptr;
	QPushButton *clearButton = nullptr;

	std::vector<Hymn> hymns;
	std::vector<int> filteredIndices;

	int currentHymnIndex = -1;
	int currentVerseIndex = -1;
	bool showingChorus = false;
	bool dockedOnce = false;
	bool dockSignalsConnected = false;

	std::string hymnalFolder;
};

void RegisterHymnalDock();
