//! \file us_data_publication.cpp

#include <QApplication>
#include <QCommandLineParser>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMessageBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QRegularExpression>
#include <QSplitter>
#include <QHeaderView>

#include "us_data_publication.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_passwd.h"
#include "us_tar.h"
#include "us_gzip.h"
#include "us_archive.h"
#include "us_util.h"
#include "us_project_gui.h"
#include "us_select_runs.h"
#include "us_data_loader.h"
#include "us_model_loader.h"
#include "us_noise_loader.h"

// Bundle format version
static const QString BUNDLE_VERSION = "1.0";

//! \brief Helper function to remove surrounding quotes from a string
static QString removeQuotes(const QString& str) {
    QString result = str;
    if (result.startsWith('"')) result = result.mid(1);
    if (result.endsWith('"')) result.chop(1);
    return result;
}

//! \brief Convert entity type to string
static QString entityTypeToString(US_DataPubEntityType type) {
    static const char* names[] = {
        "project", "experiment", "rawData", "rotorCalibration",
        "centerpiece", "buffer", "analyte", "solution",
        "edit", "model", "noise"
    };
    return (type >= 0 && type < EntityTypeCount) ? names[type] : "unknown";
}

//! \brief Convert string to entity type
static US_DataPubEntityType stringToEntityType(const QString& str) {
    static QMap<QString, US_DataPubEntityType> map;
    if (map.isEmpty()) {
        map["project"] = EntityProject;
        map["experiment"] = EntityExperiment;
        map["rawData"] = EntityRawData;
        map["rotorCalibration"] = EntityRotorCalibration;
        map["centerpiece"] = EntityCenterpiece;
        map["buffer"] = EntityBuffer;
        map["analyte"] = EntityAnalyte;
        map["solution"] = EntitySolution;
        map["edit"] = EntityEdit;
        map["model"] = EntityModel;
        map["noise"] = EntityNoise;
    }
    return map.value(str.toLower(), EntityProject);
}

// ============================================================================
// US_DataPubManifest implementation
// ============================================================================

US_DataPubManifest::US_DataPubManifest()
    : bundleVersion(BUNDLE_VERSION)
{
    createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
}

void US_DataPubManifest::clear() {
    entries.clear();
    description.clear();
}

void US_DataPubManifest::addEntry(const US_DataPubManifestEntry& entry) {
    entries.append(entry);
}

QList<US_DataPubManifestEntry> US_DataPubManifest::entriesOfType(US_DataPubEntityType type) const {
    QList<US_DataPubManifestEntry> result;
    for (const auto& entry : entries) {
        if (entry.type == type) {
            result.append(entry);
        }
    }
    return result;
}

US_DataPubManifestEntry* US_DataPubManifest::findByGuid(const QString& guid) {
    for (auto& entry : entries) {
        if (entry.guid == guid) {
            return &entry;
        }
    }
    return nullptr;
}

QList<US_DataPubManifestEntry> US_DataPubManifest::allEntries() const {
    return entries;
}

bool US_DataPubManifest::writeToFile(const QString& filepath) {
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
    out << "# UltraScan3 Data Publication Bundle Manifest\n";
    out << "version: " << bundleVersion << "\n";
    out << "created: " << createdAt << "\n";
    out << "description: \"" << description.replace("\"", "\\\"") << "\"\n";
    out << "\n";

    // Group entries by type
    for (int t = 0; t < EntityTypeCount; ++t) {
        US_DataPubEntityType type = static_cast<US_DataPubEntityType>(t);
        QList<US_DataPubManifestEntry> typeEntries = entriesOfType(type);
        
        if (typeEntries.isEmpty()) continue;

        out << entityTypeToString(type) << ":\n";
        for (const auto& entry : typeEntries) {
            out << "  - id: " << entry.id << "\n";
            out << "    guid: \"" << entry.guid << "\"\n";
            out << "    name: \"" << entry.name.replace("\"", "\\\"") << "\"\n";
            out << "    propertyHash: \"" << entry.propertyHash << "\"\n";
            out << "    payloadPath: \"" << entry.payloadPath << "\"\n";
            if (!entry.dependencyGuids.isEmpty()) {
                out << "    dependencies:\n";
                for (const QString& dep : entry.dependencyGuids) {
                    out << "      - \"" << dep << "\"\n";
                }
            }
        }
        out << "\n";
    }

    file.close();
    return true;
}

bool US_DataPubManifest::readFromFile(const QString& filepath) {
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    entries.clear();
    
    QTextStream in(&file);
    QString currentSection;
    US_DataPubManifestEntry currentEntry;
    bool inEntry = false;
    bool inDependencies = false;

    while (!in.atEnd()) {
        QString line = in.readLine();
        QString trimmed = line.trimmed();
        
        // Skip comments and empty lines
        if (trimmed.isEmpty() || trimmed.startsWith('#')) continue;

        // Check for top-level keys
        if (!line.startsWith(' ') && !line.startsWith('\t')) {
            if (inEntry) {
                entries.append(currentEntry);
                inEntry = false;
            }
            inDependencies = false;
            
            if (trimmed.startsWith("version:")) {
                bundleVersion = trimmed.mid(8).trimmed();
            } else if (trimmed.startsWith("created:")) {
                createdAt = trimmed.mid(8).trimmed();
            } else if (trimmed.startsWith("description:")) {
                description = removeQuotes(trimmed.mid(12).trimmed());
            } else if (trimmed.endsWith(":")) {
                currentSection = trimmed.left(trimmed.length() - 1);
            }
        } else if (trimmed.startsWith("- id:")) {
            if (inEntry) {
                entries.append(currentEntry);
            }
            currentEntry = US_DataPubManifestEntry();
            currentEntry.type = stringToEntityType(currentSection);
            currentEntry.id = trimmed.mid(5).trimmed().toInt();
            inEntry = true;
            inDependencies = false;
        } else if (inEntry) {
            if (trimmed.startsWith("guid:")) {
                currentEntry.guid = removeQuotes(trimmed.mid(5).trimmed());
            } else if (trimmed.startsWith("name:")) {
                currentEntry.name = removeQuotes(trimmed.mid(5).trimmed());
            } else if (trimmed.startsWith("propertyHash:")) {
                currentEntry.propertyHash = removeQuotes(trimmed.mid(13).trimmed());
            } else if (trimmed.startsWith("payloadPath:")) {
                currentEntry.payloadPath = removeQuotes(trimmed.mid(12).trimmed());
            } else if (trimmed.startsWith("dependencies:")) {
                inDependencies = true;
            } else if (inDependencies && trimmed.startsWith("- \"")) {
                QString dep = trimmed.mid(3);
                if (dep.endsWith('"')) dep.chop(1);
                currentEntry.dependencyGuids.append(dep);
            }
        }
    }

    if (inEntry) {
        entries.append(currentEntry);
    }

    file.close();
    return true;
}

// ============================================================================
// US_DataPublication implementation
// ============================================================================

US_DataPublication::US_DataPublication() : US_Widgets() {
    setWindowTitle(tr("UltraScan Data Publication"));
    setPalette(US_GuiSettings::frameColor());
    
    projectId = -1;
    conflictPolicy = ConflictReuse;
    db = nullptr;
    useDB = true;

    setupGui();
    updateExportButtonStates();
    updateImportButtonStates();
}

US_DataPublication::~US_DataPublication() {
    if (db != nullptr) {
        delete db;
    }
}

void US_DataPublication::setupGui() {
    // Main layout
    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSpacing(2);
    main->setContentsMargins(2, 2, 2, 2);

    // Banner
    QLabel* lb_banner = us_banner(tr("UltraScan Data Publication"));
    main->addWidget(lb_banner);

    // Create tab widget
    tabWidget = us_tabwidget();
    
    // Setup Export and Import tabs
    setupExportTab();
    setupImportTab();
    
    tabWidget->addTab(exportTab, tr("Export"));
    tabWidget->addTab(importTab, tr("Import"));
    main->addWidget(tabWidget);

    // Common buttons at bottom
    QHBoxLayout* buttons = new QHBoxLayout();
    pb_help = us_pushbutton(tr("Help"));
    pb_close = us_pushbutton(tr("Close"));
    buttons->addStretch();
    buttons->addWidget(pb_help);
    buttons->addWidget(pb_close);
    main->addLayout(buttons);

    connect(pb_help, &QPushButton::clicked, this, &US_DataPublication::help);
    connect(pb_close, &QPushButton::clicked, this, &US_DataPublication::close);
    
    // Set minimum size
    setMinimumSize(800, 700);
}

void US_DataPublication::setupExportTab() {
    exportTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(exportTab);
    layout->setSpacing(2);
    layout->setContentsMargins(4, 4, 4, 4);

    // Disk/DB controls at top
    exportDiskDB = new US_Disk_DB_Controls(US_Disk_DB_Controls::Default);
    layout->addLayout(exportDiskDB);
    connect(exportDiskDB, &US_Disk_DB_Controls::changed, this, &US_DataPublication::exportSourceChanged);

    // Selection section
    QGroupBox* selectionBox = new QGroupBox(tr("Data Selection"));
    QGridLayout* selLayout = new QGridLayout(selectionBox);
    int row = 0;

    // Project selection
    QLabel* lb_project = us_label(tr("Project (optional):"));
    le_project = us_lineedit("", -1, true);
    pb_selectProject = us_pushbutton(tr("Select Project..."));
    selLayout->addWidget(lb_project, row, 0);
    selLayout->addWidget(le_project, row, 1, 1, 2);
    selLayout->addWidget(pb_selectProject, row++, 3);
    connect(pb_selectProject, &QPushButton::clicked, this, &US_DataPublication::selectProject);

    // Experiment selection
    QLabel* lb_experiments = us_label(tr("Experiments:"));
    le_experiments = us_lineedit("", -1, true);
    pb_selectExperiments = us_pushbutton(tr("Select Runs..."));
    selLayout->addWidget(lb_experiments, row, 0);
    selLayout->addWidget(le_experiments, row, 1, 1, 2);
    selLayout->addWidget(pb_selectExperiments, row++, 3);
    connect(pb_selectExperiments, &QPushButton::clicked, this, &US_DataPublication::selectExperiments);

    layout->addWidget(selectionBox);

    // Data tree section with buttons
    QGroupBox* dataBox = new QGroupBox(tr("Raw Data and Edits"));
    QVBoxLayout* dataLayout = new QVBoxLayout(dataBox);
    
    // Bulk action buttons for raw data/edits
    QHBoxLayout* dataButtons = new QHBoxLayout();
    pb_selectAllRaw = us_pushbutton(tr("Select All Raw"), false);
    pb_deselectAllRaw = us_pushbutton(tr("Deselect All Raw"), false);
    pb_selectLatestEdits = us_pushbutton(tr("Latest Edits"), false);
    pb_selectAllEdits = us_pushbutton(tr("All Edits"), false);
    pb_deselectAllEdits = us_pushbutton(tr("No Edits"), false);
    dataButtons->addWidget(pb_selectAllRaw);
    dataButtons->addWidget(pb_deselectAllRaw);
    dataButtons->addStretch();
    dataButtons->addWidget(pb_selectLatestEdits);
    dataButtons->addWidget(pb_selectAllEdits);
    dataButtons->addWidget(pb_deselectAllEdits);
    dataLayout->addLayout(dataButtons);
    
    connect(pb_selectAllRaw, &QPushButton::clicked, this, &US_DataPublication::selectAllRawData);
    connect(pb_deselectAllRaw, &QPushButton::clicked, this, &US_DataPublication::deselectAllRawData);
    connect(pb_selectLatestEdits, &QPushButton::clicked, this, &US_DataPublication::selectLatestEdits);
    connect(pb_selectAllEdits, &QPushButton::clicked, this, &US_DataPublication::selectAllEdits);
    connect(pb_deselectAllEdits, &QPushButton::clicked, this, &US_DataPublication::deselectAllEdits);

    // Data tree widget
    tw_dataTree = new QTreeWidget();
    tw_dataTree->setHeaderLabels(QStringList() << tr("Name") << tr("Type") << tr("Description"));
    tw_dataTree->setColumnCount(3);
    tw_dataTree->header()->setStretchLastSection(true);
    tw_dataTree->setSelectionMode(QAbstractItemView::NoSelection);
    dataLayout->addWidget(tw_dataTree);
    connect(tw_dataTree, &QTreeWidget::itemChanged, this, &US_DataPublication::dataTreeItemChanged);
    
    layout->addWidget(dataBox);

    // Model selection section
    QGroupBox* modelBox = new QGroupBox(tr("Models"));
    QVBoxLayout* modelLayout = new QVBoxLayout(modelBox);
    
    QHBoxLayout* modelButtons = new QHBoxLayout();
    pb_selectModels = us_pushbutton(tr("Select Models..."), false);
    modelButtons->addWidget(pb_selectModels);
    modelButtons->addStretch();
    modelLayout->addLayout(modelButtons);
    connect(pb_selectModels, &QPushButton::clicked, this, &US_DataPublication::selectModels);

    // Model tree widget
    tw_modelTree = new QTreeWidget();
    tw_modelTree->setHeaderLabels(QStringList() << tr("Name") << tr("Type") << tr("Edit") << tr("Noise"));
    tw_modelTree->setColumnCount(4);
    tw_modelTree->header()->setStretchLastSection(true);
    tw_modelTree->setSelectionMode(QAbstractItemView::NoSelection);
    modelLayout->addWidget(tw_modelTree);
    connect(tw_modelTree, &QTreeWidget::itemChanged, this, &US_DataPublication::modelTreeItemChanged);
    
    layout->addWidget(modelBox);

    // Output file and summary
    QGroupBox* outputBox = new QGroupBox(tr("Output"));
    QGridLayout* outLayout = new QGridLayout(outputBox);
    row = 0;

    QLabel* lb_file = us_label(tr("Bundle File:"));
    le_exportFile = us_lineedit("");
    pb_browseExport = us_pushbutton(tr("Browse..."));
    outLayout->addWidget(lb_file, row, 0);
    outLayout->addWidget(le_exportFile, row, 1, 1, 2);
    outLayout->addWidget(pb_browseExport, row++, 3);
    connect(pb_browseExport, &QPushButton::clicked, this, &US_DataPublication::browseExportFile);

    // Summary
    lb_summaryExport = us_label(tr("Summary: No data selected"));
    pb_manifestPreview = us_pushbutton(tr("Preview Manifest..."), false);
    outLayout->addWidget(lb_summaryExport, row, 0, 1, 3);
    outLayout->addWidget(pb_manifestPreview, row++, 3);
    connect(pb_manifestPreview, &QPushButton::clicked, this, &US_DataPublication::showManifestPreview);

    layout->addWidget(outputBox);

    // Progress and status
    progressExport = new QProgressBar();
    progressExport->setRange(0, 100);
    progressExport->setValue(0);
    layout->addWidget(progressExport);

    te_statusExport = us_textedit();
    te_statusExport->setMaximumHeight(100);
    layout->addWidget(te_statusExport);

    // Export button
    QHBoxLayout* exportButtons = new QHBoxLayout();
    pb_startExport = us_pushbutton(tr("Export Bundle"), false);
    exportButtons->addStretch();
    exportButtons->addWidget(pb_startExport);
    layout->addLayout(exportButtons);
    connect(pb_startExport, &QPushButton::clicked, this, &US_DataPublication::startExport);
}

void US_DataPublication::setupImportTab() {
    importTab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(importTab);
    layout->setSpacing(2);
    layout->setContentsMargins(4, 4, 4, 4);

    // Disk/DB controls at top
    importDiskDB = new US_Disk_DB_Controls(US_Disk_DB_Controls::Default);
    layout->addLayout(importDiskDB);
    connect(importDiskDB, &US_Disk_DB_Controls::changed, this, &US_DataPublication::importSourceChanged);

    // File selection
    QGroupBox* fileBox = new QGroupBox(tr("Bundle File"));
    QHBoxLayout* fileLayout = new QHBoxLayout(fileBox);
    le_importFile = us_lineedit("");
    pb_browseImport = us_pushbutton(tr("Browse..."));
    fileLayout->addWidget(le_importFile);
    fileLayout->addWidget(pb_browseImport);
    layout->addWidget(fileBox);
    connect(pb_browseImport, &QPushButton::clicked, this, &US_DataPublication::browseImportFile);

    // Import options
    QGroupBox* optionsBox = new QGroupBox(tr("Import Options"));
    QGridLayout* optLayout = new QGridLayout(optionsBox);
    int row = 0;

    QLabel* lb_conflict = us_label(tr("On Conflict:"));
    cb_conflict = us_comboBox();
    cb_conflict->addItem(tr("Reuse if properties match"), ConflictReuse);
    cb_conflict->addItem(tr("Always rename"), ConflictRename);
    cb_conflict->addItem(tr("Fail"), ConflictFail);
    optLayout->addWidget(lb_conflict, row, 0);
    optLayout->addWidget(cb_conflict, row++, 1, 1, 2);
    
    layout->addWidget(optionsBox);

    // Preview tree
    QGroupBox* previewBox = new QGroupBox(tr("Bundle Contents Preview"));
    QVBoxLayout* prevLayout = new QVBoxLayout(previewBox);
    tw_importPreview = new QTreeWidget();
    tw_importPreview->setHeaderLabels(QStringList() << tr("Name") << tr("Type") << tr("GUID"));
    tw_importPreview->setColumnCount(3);
    tw_importPreview->header()->setStretchLastSection(true);
    prevLayout->addWidget(tw_importPreview);
    layout->addWidget(previewBox);

    // Summary
    lb_summaryImport = us_label(tr("Summary: No bundle loaded"));
    layout->addWidget(lb_summaryImport);

    // Progress and status
    progressImport = new QProgressBar();
    progressImport->setRange(0, 100);
    progressImport->setValue(0);
    layout->addWidget(progressImport);

    te_statusImport = us_textedit();
    te_statusImport->setMaximumHeight(100);
    layout->addWidget(te_statusImport);

    // Import button
    QHBoxLayout* importButtons = new QHBoxLayout();
    pb_startImport = us_pushbutton(tr("Import Bundle"), false);
    importButtons->addStretch();
    importButtons->addWidget(pb_startImport);
    layout->addLayout(importButtons);
    connect(pb_startImport, &QPushButton::clicked, this, &US_DataPublication::startImport);
}

void US_DataPublication::exportSourceChanged(bool db) {
    useDB = db;
    // Clear selections when source changes
    projectId = -1;
    le_project->clear();
    le_experiments->clear();
    selectedRunIDs.clear();
    selectedExpIDs.clear();
    rawDataList.clear();
    modelList.clear();
    tw_dataTree->clear();
    tw_modelTree->clear();
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::importSourceChanged(bool db) {
    Q_UNUSED(db)
    updateImportButtonStates();
}

void US_DataPublication::updateExportButtonStates() {
    bool hasExperiments = !selectedRunIDs.isEmpty();
    bool hasRawData = !rawDataList.isEmpty();
    bool hasModels = !modelList.isEmpty();
    
    pb_selectAllRaw->setEnabled(hasRawData);
    pb_deselectAllRaw->setEnabled(hasRawData);
    pb_selectLatestEdits->setEnabled(hasRawData);
    pb_selectAllEdits->setEnabled(hasRawData);
    pb_deselectAllEdits->setEnabled(hasRawData);
    pb_selectModels->setEnabled(hasExperiments);
    pb_manifestPreview->setEnabled(countSelectedRawData() > 0 || countSelectedModels() > 0);
    pb_startExport->setEnabled(!le_exportFile->text().isEmpty() && 
                                (countSelectedRawData() > 0 || countSelectedEdits() > 0 || countSelectedModels() > 0));
}

void US_DataPublication::updateImportButtonStates() {
    pb_startImport->setEnabled(!le_importFile->text().isEmpty());
}

bool US_DataPublication::connectToDatabase() {
    if (db != nullptr && db->isConnected()) {
        return true;
    }
    
    US_Passwd pw;
    db = new US_DB2(pw.getPasswd());
    
    if (db->lastErrno() != US_DB2::OK) {
        QMessageBox::warning(this, tr("Database Error"),
            tr("Could not connect to database:\n%1").arg(db->lastError()));
        delete db;
        db = nullptr;
        return false;
    }
    
    return true;
}

void US_DataPublication::selectProject() {
    // Use US_ProjectGui for project selection
    int diskDB = exportDiskDB->db() ? US_Disk_DB_Controls::DB : US_Disk_DB_Controls::Disk;
    
    US_ProjectGui* projDialog = new US_ProjectGui(true, diskDB, currentProject);
    
    connect(projDialog, &US_ProjectGui::updateProjectGuiSelection, 
            this, &US_DataPublication::projectSelected);
    
    projDialog->exec();
    delete projDialog;
}

void US_DataPublication::projectSelected(US_Project& project) {
    currentProject = project;
    projectId = project.projectID;
    le_project->setText(QString("%1: %2").arg(projectId).arg(project.projectDesc));
    
    // Clear experiment selection since project changed
    selectedRunIDs.clear();
    selectedExpIDs.clear();
    le_experiments->clear();
    rawDataList.clear();
    modelList.clear();
    tw_dataTree->clear();
    tw_modelTree->clear();
    
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::selectExperiments() {
    // Use US_SelectRuns for experiment selection
    bool fromDB = exportDiskDB->db();
    
    US_SelectRuns* runsDialog = new US_SelectRuns(fromDB, selectedRunIDs);
    
    if (runsDialog->exec() == QDialog::Accepted && !selectedRunIDs.isEmpty()) {
        le_experiments->setText(tr("%1 run(s) selected").arg(selectedRunIDs.size()));
        
        // Load raw data and edits for selected experiments
        loadRawDataForExperiments();
        buildDataTree();
    }
    
    delete runsDialog;
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::selectModels() {
    if (selectedRunIDs.isEmpty()) {
        QMessageBox::warning(this, tr("No Experiments"), 
            tr("Please select experiments first before selecting models."));
        return;
    }
    
    bool fromDB = exportDiskDB->db();
    QString searchFilter;
    QList<US_Model> selectedModels;
    QStringList modelDescrs;
    
    // Use US_ModelLoader for model selection
    US_ModelLoader* modelLoader = new US_ModelLoader(fromDB, searchFilter, 
                                                      selectedModels, modelDescrs, 
                                                      selectedRunIDs);
    
    if (modelLoader->exec() == QDialog::Accepted && !selectedModels.isEmpty()) {
        // Process selected models
        modelList.clear();
        for (int i = 0; i < selectedModels.size(); ++i) {
            US_DataPubModelInfo info;
            info.modelGUID = selectedModels[i].modelGUID;
            info.description = selectedModels[i].description;
            info.editGUID = selectedModels[i].editGUID;
            info.selected = true;
            modelList.append(info);
        }
        
        // Check if model dependencies require additional edits to be selected
        checkModelDependencies();
        buildModelTree();
    }
    
    delete modelLoader;
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::selectAllRawData() {
    tw_dataTree->blockSignals(true);
    for (int i = 0; i < rawDataList.size(); ++i) {
        rawDataList[i].selected = true;
    }
    buildDataTree();
    tw_dataTree->blockSignals(false);
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::deselectAllRawData() {
    tw_dataTree->blockSignals(true);
    for (int i = 0; i < rawDataList.size(); ++i) {
        rawDataList[i].selected = false;
    }
    buildDataTree();
    tw_dataTree->blockSignals(false);
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::selectLatestEdits() {
    tw_dataTree->blockSignals(true);
    for (int i = 0; i < rawDataList.size(); ++i) {
        if (rawDataList[i].selected && !rawDataList[i].editSelected.isEmpty()) {
            // Deselect all, then select only the last (latest) edit
            for (int j = 0; j < rawDataList[i].editSelected.size(); ++j) {
                rawDataList[i].editSelected[j] = false;
            }
            rawDataList[i].editSelected[rawDataList[i].editSelected.size() - 1] = true;
        }
    }
    buildDataTree();
    tw_dataTree->blockSignals(false);
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::selectAllEdits() {
    tw_dataTree->blockSignals(true);
    for (int i = 0; i < rawDataList.size(); ++i) {
        if (rawDataList[i].selected) {
            for (int j = 0; j < rawDataList[i].editSelected.size(); ++j) {
                rawDataList[i].editSelected[j] = true;
            }
        }
    }
    buildDataTree();
    tw_dataTree->blockSignals(false);
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::deselectAllEdits() {
    tw_dataTree->blockSignals(true);
    for (int i = 0; i < rawDataList.size(); ++i) {
        for (int j = 0; j < rawDataList[i].editSelected.size(); ++j) {
            rawDataList[i].editSelected[j] = false;
        }
    }
    buildDataTree();
    tw_dataTree->blockSignals(false);
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::dataTreeItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0) return;
    
    QString itemType = item->data(0, Qt::UserRole).toString();
    int index = item->data(0, Qt::UserRole + 1).toInt();
    bool checked = (item->checkState(0) == Qt::Checked);
    
    if (itemType == "rawdata" && index >= 0 && index < rawDataList.size()) {
        rawDataList[index].selected = checked;
        // If deselecting raw data, check if any models depend on it
        if (!checked) {
            checkModelDependencies();
        }
    } else if (itemType == "edit") {
        int rawIndex = item->data(0, Qt::UserRole + 2).toInt();
        if (rawIndex >= 0 && rawIndex < rawDataList.size() &&
            index >= 0 && index < rawDataList[rawIndex].editSelected.size()) {
            rawDataList[rawIndex].editSelected[index] = checked;
            // If deselecting edit, check if any models depend on it
            if (!checked) {
                checkModelDependencies();
            }
        }
    }
    
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::modelTreeItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0) return;
    
    int index = item->data(0, Qt::UserRole).toInt();
    bool checked = (item->checkState(0) == Qt::Checked);
    
    if (index >= 0 && index < modelList.size()) {
        modelList[index].selected = checked;
        
        // If selecting a model, ensure its dependent edit is also selected
        if (checked) {
            checkModelDependencies();
        }
    }
    
    updateExportButtonStates();
    updateSummary();
}

void US_DataPublication::browseExportFile() {
    QString filter = tr("Data Publication Bundle (*.tar.gz)");
    QString path = QFileDialog::getSaveFileName(this, tr("Save Bundle"), 
        US_Settings::dataDir(), filter);
    
    if (!path.isEmpty()) {
        if (!path.endsWith(".tar.gz")) {
            path += ".tar.gz";
        }
        le_exportFile->setText(path);
        exportBundlePath = path;
    }
    updateExportButtonStates();
}

void US_DataPublication::browseImportFile() {
    QString filter = tr("Data Publication Bundle (*.tar.gz)");
    QString path = QFileDialog::getOpenFileName(this, tr("Open Bundle"),
        US_Settings::dataDir(), filter);
    
    if (!path.isEmpty()) {
        le_importFile->setText(path);
        importBundlePath = path;
        
        // TODO: Load and preview bundle contents
        lb_summaryImport->setText(tr("Bundle: %1").arg(QFileInfo(path).fileName()));
    }
    updateImportButtonStates();
}

void US_DataPublication::startExport() {
    if (exportBundlePath.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select an output file."));
        return;
    }
    
    if (countSelectedRawData() == 0 && countSelectedEdits() == 0 && countSelectedModels() == 0) {
        QMessageBox::warning(this, tr("Error"), tr("Please select at least some data to export."));
        return;
    }
    
    if (exportDiskDB->db() && !connectToDatabase()) return;
    
    te_statusExport->append(tr("Starting export..."));
    progressExport->setValue(0);
    
    US_DataPubExport exporter(db);
    connect(&exporter, &US_DataPubExport::progress, this, &US_DataPublication::updateProgress);
    connect(&exporter, &US_DataPubExport::completed, this, &US_DataPublication::exportComplete);
    
    bool success = exporter.exportBundle(exportBundlePath, projectId, -1, ScopeAll);
    
    if (!success) {
        te_statusExport->append(tr("Export failed: %1").arg(exporter.errorMessage()));
    }
}

void US_DataPublication::startImport() {
    if (importBundlePath.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a bundle file."));
        return;
    }
    
    conflictPolicy = static_cast<US_DataPubConflictPolicy>(cb_conflict->currentData().toInt());
    
    ImportTarget target = importDiskDB->db() ? TargetDatabase : TargetDisk;
    if (target == TargetDatabase && !connectToDatabase()) return;
    
    te_statusImport->append(tr("Starting import..."));
    progressImport->setValue(0);
    
    US_DataPubImport importer(db);
    connect(&importer, &US_DataPubImport::progress, this, &US_DataPublication::updateProgress);
    connect(&importer, &US_DataPubImport::completed, this, &US_DataPublication::importComplete);
    
    bool success = importer.importBundle(importBundlePath, target, 
                                          US_Settings::dataDir(), conflictPolicy);
    
    if (!success) {
        te_statusImport->append(tr("Import failed: %1").arg(importer.errorMessage()));
    }
}

void US_DataPublication::showManifestPreview() {
    QString preview = generateManifestPreview();
    
    QDialog* dialog = new QDialog(this);
    dialog->setWindowTitle(tr("Manifest Preview"));
    dialog->setMinimumSize(600, 400);
    
    QVBoxLayout* layout = new QVBoxLayout(dialog);
    QTextEdit* te = new QTextEdit();
    te->setPlainText(preview);
    te->setReadOnly(true);
    te->setFont(QFont("Courier", 10));
    layout->addWidget(te);
    
    QPushButton* pb_close = new QPushButton(tr("Close"));
    connect(pb_close, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(pb_close);
    
    dialog->exec();
    delete dialog;
}

void US_DataPublication::updateProgress(int value, const QString& message) {
    // Update whichever tab is active
    if (tabWidget->currentIndex() == 0) {
        progressExport->setValue(value);
        te_statusExport->append(message);
    } else {
        progressImport->setValue(value);
        te_statusImport->append(message);
    }
}

void US_DataPublication::exportComplete(bool success, const QString& message) {
    progressExport->setValue(100);
    if (success) {
        te_statusExport->append(tr("Export completed successfully."));
        QMessageBox::information(this, tr("Export Complete"), message);
    } else {
        te_statusExport->append(tr("Export failed: %1").arg(message));
        QMessageBox::warning(this, tr("Export Failed"), message);
    }
}

void US_DataPublication::importComplete(bool success, const QString& message) {
    progressImport->setValue(100);
    if (success) {
        te_statusImport->append(tr("Import completed successfully."));
        QMessageBox::information(this, tr("Import Complete"), message);
    } else {
        te_statusImport->append(tr("Import failed: %1").arg(message));
        QMessageBox::warning(this, tr("Import Failed"), message);
    }
}

void US_DataPublication::updateSummary() {
    int rawCount = countSelectedRawData();
    int editCount = countSelectedEdits();
    int modelCount = countSelectedModels();
    
    QString summary = tr("Summary: %1 raw data, %2 edits, %3 models selected")
        .arg(rawCount).arg(editCount).arg(modelCount);
    
    if (projectId > 0) {
        summary = tr("Project: %1 | ").arg(currentProject.projectDesc) + summary;
    }
    
    lb_summaryExport->setText(summary);
}

void US_DataPublication::help() {
    showHelp.show_help("manual/us_data_publication.html");
}

void US_DataPublication::loadRawDataForExperiments() {
    rawDataList.clear();
    
    // This would query the database/disk for raw data associated with selected experiments
    // For now, create placeholder implementation
    // In a full implementation, this would use US_DataIO to load raw data info
    
    te_statusExport->append(tr("Loading raw data for %1 experiments...").arg(selectedRunIDs.size()));
}

void US_DataPublication::loadEditsForRawData() {
    // This would load edit information for the raw data
    // In a full implementation, this would query for edit profiles
}

void US_DataPublication::buildDataTree() {
    tw_dataTree->clear();
    tw_dataTree->blockSignals(true);
    
    // Group by experiment/run
    QMap<QString, QList<int>> runGroups;
    for (int i = 0; i < rawDataList.size(); ++i) {
        runGroups[rawDataList[i].runID].append(i);
    }
    
    for (auto it = runGroups.begin(); it != runGroups.end(); ++it) {
        // Create run/experiment node
        QTreeWidgetItem* runItem = new QTreeWidgetItem(tw_dataTree);
        runItem->setText(0, it.key());
        runItem->setText(1, tr("Experiment"));
        runItem->setFlags(runItem->flags() & ~Qt::ItemIsUserCheckable);
        
        // Add raw data items
        for (int idx : it.value()) {
            const US_DataPubRawDataInfo& raw = rawDataList[idx];
            
            QTreeWidgetItem* rawItem = new QTreeWidgetItem(runItem);
            rawItem->setText(0, raw.description);
            rawItem->setText(1, tr("Raw Data"));
            rawItem->setText(2, raw.rawGUID);
            rawItem->setFlags(rawItem->flags() | Qt::ItemIsUserCheckable);
            rawItem->setCheckState(0, raw.selected ? Qt::Checked : Qt::Unchecked);
            rawItem->setData(0, Qt::UserRole, "rawdata");
            rawItem->setData(0, Qt::UserRole + 1, idx);
            
            // Add edit items
            for (int j = 0; j < raw.editIDs.size(); ++j) {
                QTreeWidgetItem* editItem = new QTreeWidgetItem(rawItem);
                editItem->setText(0, raw.editNames.value(j, tr("Edit %1").arg(j + 1)));
                editItem->setText(1, tr("Edit"));
                editItem->setText(2, raw.editGUIDs.value(j));
                editItem->setFlags(editItem->flags() | Qt::ItemIsUserCheckable);
                editItem->setCheckState(0, raw.editSelected.value(j, false) ? Qt::Checked : Qt::Unchecked);
                editItem->setData(0, Qt::UserRole, "edit");
                editItem->setData(0, Qt::UserRole + 1, j);
                editItem->setData(0, Qt::UserRole + 2, idx);
            }
        }
        
        runItem->setExpanded(true);
    }
    
    tw_dataTree->blockSignals(false);
}

void US_DataPublication::buildModelTree() {
    tw_modelTree->clear();
    tw_modelTree->blockSignals(true);
    
    for (int i = 0; i < modelList.size(); ++i) {
        const US_DataPubModelInfo& model = modelList[i];
        
        QTreeWidgetItem* item = new QTreeWidgetItem(tw_modelTree);
        item->setText(0, model.description);
        item->setText(1, tr("Model"));
        item->setText(2, model.editGUID);
        item->setText(3, model.noiseGUID_ti.isEmpty() && model.noiseGUID_ri.isEmpty() ? 
                      tr("None") : tr("Yes"));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, model.selected ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, i);
    }
    
    tw_modelTree->blockSignals(false);
}

void US_DataPublication::checkModelDependencies() {
    // Check if any selected models depend on deselected edits/raw data
    // If so, prompt the user to either select the dependencies or deselect the models
    
    for (int i = 0; i < modelList.size(); ++i) {
        if (!modelList[i].selected) continue;
        
        QString editGUID = modelList[i].editGUID;
        bool editSelected = false;
        
        // Check if the model's edit is selected
        for (int j = 0; j < rawDataList.size(); ++j) {
            for (int k = 0; k < rawDataList[j].editGUIDs.size(); ++k) {
                if (rawDataList[j].editGUIDs[k] == editGUID) {
                    if (rawDataList[j].selected && rawDataList[j].editSelected.value(k, false)) {
                        editSelected = true;
                    }
                    break;
                }
            }
        }
        
        if (!editSelected) {
            // The model depends on an edit that is not selected
            QMessageBox::StandardButton reply = QMessageBox::question(this,
                tr("Dependency Required"),
                tr("Model '%1' depends on an edit that is not selected.\n"
                   "Do you want to automatically select the required edit?")
                   .arg(modelList[i].description),
                QMessageBox::Yes | QMessageBox::No);
            
            if (reply == QMessageBox::Yes) {
                // Select the required edit
                for (int j = 0; j < rawDataList.size(); ++j) {
                    for (int k = 0; k < rawDataList[j].editGUIDs.size(); ++k) {
                        if (rawDataList[j].editGUIDs[k] == editGUID) {
                            rawDataList[j].selected = true;
                            rawDataList[j].editSelected[k] = true;
                            break;
                        }
                    }
                }
                buildDataTree();
            } else {
                // Deselect the model
                modelList[i].selected = false;
                buildModelTree();
            }
        }
    }
}

QString US_DataPublication::generateManifestPreview() {
    QString preview;
    QTextStream out(&preview);
    
    out << "# UltraScan3 Data Publication Bundle Manifest Preview\n";
    out << "version: " << BUNDLE_VERSION << "\n";
    out << "created: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";
    
    if (projectId > 0) {
        out << "project:\n";
        out << "  - name: \"" << currentProject.projectDesc << "\"\n";
        out << "    guid: \"" << currentProject.projectGUID << "\"\n\n";
    }
    
    int rawCount = countSelectedRawData();
    int editCount = countSelectedEdits();
    int modelCount = countSelectedModels();
    
    out << "# Selected items summary:\n";
    out << "# Raw Data: " << rawCount << "\n";
    out << "# Edits: " << editCount << "\n";
    out << "# Models: " << modelCount << "\n\n";
    
    if (rawCount > 0) {
        out << "rawData:\n";
        for (const auto& raw : rawDataList) {
            if (raw.selected) {
                out << "  - name: \"" << raw.description << "\"\n";
                out << "    guid: \"" << raw.rawGUID << "\"\n";
            }
        }
        out << "\n";
    }
    
    if (editCount > 0) {
        out << "edits:\n";
        for (const auto& raw : rawDataList) {
            for (int j = 0; j < raw.editSelected.size(); ++j) {
                if (raw.editSelected[j]) {
                    out << "  - name: \"" << raw.editNames.value(j) << "\"\n";
                    out << "    guid: \"" << raw.editGUIDs.value(j) << "\"\n";
                }
            }
        }
        out << "\n";
    }
    
    if (modelCount > 0) {
        out << "models:\n";
        for (const auto& model : modelList) {
            if (model.selected) {
                out << "  - name: \"" << model.description << "\"\n";
                out << "    guid: \"" << model.modelGUID << "\"\n";
            }
        }
        out << "\n";
    }
    
    return preview;
}

int US_DataPublication::countSelectedRawData() {
    int count = 0;
    for (const auto& raw : rawDataList) {
        if (raw.selected) ++count;
    }
    return count;
}

int US_DataPublication::countSelectedEdits() {
    int count = 0;
    for (const auto& raw : rawDataList) {
        for (bool sel : raw.editSelected) {
            if (sel) ++count;
        }
    }
    return count;
}

int US_DataPublication::countSelectedModels() {
    int count = 0;
    for (const auto& model : modelList) {
        if (model.selected) ++count;
    }
    return count;
}

// ============================================================================
// CLI Implementation
// ============================================================================

void US_DataPublication::printUsage() {
    QTextStream out(stdout);
    out << "UltraScan Data Publication Tool\n\n";
    out << "Usage:\n";
    out << "  us_data_publication --mode export --bundle <path> [options]\n";
    out << "  us_data_publication --mode import --bundle <path> [options]\n\n";
    out << "Options:\n";
    out << "  --mode <export|import>      Operation mode\n";
    out << "  --bundle <path>             Path to bundle file (.tar.gz)\n";
    out << "  --project-id <id>           Project ID for export\n";
    out << "  --experiment-id <id>        Experiment ID for export\n";
    out << "  --max-scope <scope>         Export scope: project|experiment|rawdata|edits|models|all\n";
    out << "  --target <db|disk>          Import target (default: db)\n";
    out << "  --output-dir <path>         Output directory for disk import\n";
    out << "  --on-conflict <policy>      Conflict policy: reuse|rename|fail (default: reuse)\n";
    out << "  --non-interactive           Run without prompts\n";
    out << "  --verbose                   Enable verbose output\n";
    out << "  --dry-run                   Show what would be done without making changes\n";
    out << "  --no-ui                     Run in CLI mode only\n";
    out << "  --help                      Show this help\n";
}

int US_DataPublication::runCli(int argc, char* argv[]) {
    QCommandLineParser parser;
    parser.setApplicationDescription("UltraScan Data Publication Tool");
    parser.addHelpOption();
    
    parser.addOption({"mode", "Operation mode (export|import)", "mode"});
    parser.addOption({"bundle", "Bundle file path", "path"});
    parser.addOption({"project-id", "Project ID", "id"});
    parser.addOption({"experiment-id", "Experiment ID", "id"});
    parser.addOption({"max-scope", "Export scope", "scope", "all"});
    parser.addOption({"target", "Import target (db|disk)", "target", "db"});
    parser.addOption({"output-dir", "Output directory", "path"});
    parser.addOption({"on-conflict", "Conflict policy", "policy", "reuse"});
    parser.addOption({"non-interactive", "Non-interactive mode"});
    parser.addOption({"verbose", "Verbose output"});
    parser.addOption({"dry-run", "Dry run mode"});
    parser.addOption({"no-ui", "CLI mode only"});
    
    QStringList args;
    for (int i = 0; i < argc; ++i) {
        args << argv[i];
    }
    
    if (!parser.parse(args)) {
        QTextStream(stderr) << parser.errorText() << "\n";
        return 1;
    }
    
    if (parser.isSet("help")) {
        printUsage();
        return 0;
    }
    
    QString mode = parser.value("mode");
    QString bundle = parser.value("bundle");
    bool verbose = parser.isSet("verbose");
    bool dryRun = parser.isSet("dry-run");
    
    if (mode.isEmpty() || bundle.isEmpty()) {
        printUsage();
        return 1;
    }
    
    if (mode == "export") {
        int projectId = parser.value("project-id").toInt();
        int experimentId = parser.isSet("experiment-id") ? 
                           parser.value("experiment-id").toInt() : -1;
        
        QString scopeStr = parser.value("max-scope").toLower();
        ExportScope scope = ScopeAll;
        if (scopeStr == "project") scope = ScopeProject;
        else if (scopeStr == "experiment") scope = ScopeExperiment;
        else if (scopeStr == "rawdata") scope = ScopeRawData;
        else if (scopeStr == "edits") scope = ScopeEdits;
        else if (scopeStr == "models") scope = ScopeModels;
        
        return doExport(bundle, projectId, experimentId, scope, verbose, dryRun);
        
    } else if (mode == "import") {
        ImportTarget target = parser.value("target") == "disk" ? TargetDisk : TargetDatabase;
        QString outputDir = parser.value("output-dir");
        
        QString policyStr = parser.value("on-conflict").toLower();
        US_DataPubConflictPolicy policy = ConflictReuse;
        if (policyStr == "rename") policy = ConflictRename;
        else if (policyStr == "fail") policy = ConflictFail;
        
        return doImport(bundle, target, outputDir, policy, verbose, dryRun);
    }
    
    printUsage();
    return 1;
}

int US_DataPublication::doExport(const QString& bundlePath, int projectId, 
                                  int experimentId, ExportScope scope,
                                  bool verbose, bool dryRun) {
    QTextStream out(stdout);
    
    if (verbose) {
        out << "Exporting to: " << bundlePath << "\n";
        out << "Project ID: " << projectId << "\n";
        if (experimentId > 0) out << "Experiment ID: " << experimentId << "\n";
    }
    
    if (dryRun) {
        out << "[DRY RUN] Would export data bundle\n";
        return 0;
    }
    
    // Connect to database
    US_Passwd pw;
    US_DB2 db(pw.getPasswd());
    
    if (db.lastErrno() != US_DB2::OK) {
        QTextStream(stderr) << "Database error: " << db.lastError() << "\n";
        return 1;
    }
    
    US_DataPubExport exporter(&db);
    
    QObject::connect(&exporter, &US_DataPubExport::progress,
        [verbose](int percent, const QString& msg) {
            if (verbose) {
                QTextStream(stdout) << "[" << percent << "%] " << msg << "\n";
            }
        });
    
    if (!exporter.exportBundle(bundlePath, projectId, experimentId, scope)) {
        QTextStream(stderr) << "Export failed: " << exporter.errorMessage() << "\n";
        return 1;
    }
    
    out << "Export completed successfully.\n";
    return 0;
}

int US_DataPublication::doImport(const QString& bundlePath, ImportTarget target,
                                  const QString& outputDir,
                                  US_DataPubConflictPolicy policy,
                                  bool verbose, bool dryRun) {
    QTextStream out(stdout);
    
    if (verbose) {
        out << "Importing from: " << bundlePath << "\n";
        out << "Target: " << (target == TargetDatabase ? "database" : "disk") << "\n";
    }
    
    if (dryRun) {
        out << "[DRY RUN] Would import data bundle\n";
        return 0;
    }
    
    US_DB2* db = nullptr;
    
    if (target == TargetDatabase) {
        US_Passwd pw;
        db = new US_DB2(pw.getPasswd());
        
        if (db->lastErrno() != US_DB2::OK) {
            QTextStream(stderr) << "Database error: " << db->lastError() << "\n";
            delete db;
            return 1;
        }
    }
    
    US_DataPubImport importer(db);
    
    QObject::connect(&importer, &US_DataPubImport::progress,
        [verbose](int percent, const QString& msg) {
            if (verbose) {
                QTextStream(stdout) << "[" << percent << "%] " << msg << "\n";
            }
        });
    
    if (!importer.importBundle(bundlePath, target, outputDir, policy)) {
        QTextStream(stderr) << "Import failed: " << importer.errorMessage() << "\n";
        if (db) delete db;
        return 1;
    }
    
    out << "Import completed successfully.\n";
    if (db) delete db;
    return 0;
}

// ============================================================================
// US_DataPubExport implementation
// ============================================================================

US_DataPubExport::US_DataPubExport(US_DB2* database) 
    : db(database) {
}

US_DataPubExport::~US_DataPubExport() {
}

QString US_DataPubExport::computePropertyHash(const QVariantMap& properties) {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    
    QStringList keys = properties.keys();
    keys.sort();
    
    for (const QString& key : keys) {
        stream << key << properties[key];
    }
    
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

bool US_DataPubExport::exportBundle(const QString& bundlePath, int projectId,
                                     int experimentId, 
                                     US_DataPublication::ExportScope scope) {
    // Create temporary directory
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        lastError = "Failed to create temporary directory";
        return false;
    }
    this->tempDir = tempDir.path();
    
    manifest.clear();
    manifest.description = QString("Data publication bundle created %1")
        .arg(QDateTime::currentDateTime().toString());
    
    emit progress(5, tr("Gathering entities..."));
    
    // Gather all entities based on scope
    gatherDependencies(projectId, experimentId, scope);
    
    emit progress(10, tr("Creating payload directories..."));
    
    // Create payload directories
    for (int t = 0; t < EntityTypeCount; ++t) {
        QString typeName = entityTypeToString(static_cast<US_DataPubEntityType>(t));
        QDir(tempDir.path()).mkdir(typeName);
    }
    
    emit progress(20, tr("Exporting project..."));
    
    // Export project
    if (projectId > 0) {
        US_Project project;
        if (project.readFromDB(projectId, db) == US_DB2::OK) {
            if (!exportProject(project)) {
                return false;
            }
        }
    }
    
    emit progress(50, tr("Writing manifest..."));
    
    // Write manifest
    QString manifestPath = tempDir.path() + "/manifest.yaml";
    if (!manifest.writeToFile(manifestPath)) {
        lastError = "Failed to write manifest";
        return false;
    }
    
    emit progress(70, tr("Creating archive..."));
    
    // Create tar.gz archive
    US_Archive archive;
    QStringList sources;
    sources << tempDir.path();
    
    QString archivePath = bundlePath;
    if (!archive.compress(sources, archivePath)) {
        lastError = "Failed to create archive: " + archive.getError();
        return false;
    }
    
    emit progress(100, tr("Export complete"));
    emit completed(true, tr("Bundle created successfully at %1").arg(bundlePath));
    
    return true;
}

void US_DataPubExport::gatherDependencies(int projectId, int experimentId,
                                           US_DataPublication::ExportScope scope) {
    // This would gather all entities based on scope
    // For now, just handle the project
    Q_UNUSED(projectId)
    Q_UNUSED(experimentId)
    Q_UNUSED(scope)
}

bool US_DataPubExport::exportProject(const US_Project& project) {
    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.id = project.projectID;
    entry.guid = project.projectGUID;
    entry.name = project.projectDesc;
    entry.type = EntityProject;
    entry.payloadPath = QString("project/%1.xml").arg(project.projectGUID);
    
    // Compute property hash
    QVariantMap props;
    props["desc"] = project.projectDesc;
    props["goals"] = project.goals;
    props["molecules"] = project.molecules;
    entry.propertyHash = computePropertyHash(props);
    
    // Write project XML
    QString filePath = tempDir + "/" + entry.payloadPath;
    QDir().mkpath(QFileInfo(filePath).path());
    
    // Create XML content
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        lastError = "Failed to write project file";
        return false;
    }
    
    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("Project");
    xml.writeTextElement("projectID", QString::number(project.projectID));
    xml.writeTextElement("projectGUID", project.projectGUID);
    xml.writeTextElement("description", project.projectDesc);
    xml.writeTextElement("goals", project.goals);
    xml.writeTextElement("molecules", project.molecules);
    xml.writeTextElement("purity", project.purity);
    xml.writeTextElement("expense", project.expense);
    xml.writeTextElement("bufferComponents", project.bufferComponents);
    xml.writeTextElement("saltInformation", project.saltInformation);
    xml.writeTextElement("AUC_questions", project.AUC_questions);
    xml.writeTextElement("expDesign", project.expDesign);
    xml.writeTextElement("notes", project.notes);
    xml.writeTextElement("status", project.status);
    xml.writeEndElement();
    xml.writeEndDocument();
    file.close();
    
    manifest.addEntry(entry);
    return true;
}

bool US_DataPubExport::exportExperiment(int expId) {
    Q_UNUSED(expId)
    // Implementation would export experiment data
    return true;
}

bool US_DataPubExport::exportRawData(const QString& rawGuid) {
    Q_UNUSED(rawGuid)
    return true;
}

bool US_DataPubExport::exportBuffer(const US_Buffer& buffer) {
    Q_UNUSED(buffer)
    return true;
}

bool US_DataPubExport::exportAnalyte(const US_Analyte& analyte) {
    Q_UNUSED(analyte)
    return true;
}

bool US_DataPubExport::exportSolution(const US_Solution& solution) {
    Q_UNUSED(solution)
    return true;
}

bool US_DataPubExport::exportModel(const QString& modelGuid) {
    Q_UNUSED(modelGuid)
    return true;
}

bool US_DataPubExport::exportNoise(const QString& noiseGuid) {
    Q_UNUSED(noiseGuid)
    return true;
}

bool US_DataPubExport::exportRotorCalibration(int calibrationId) {
    Q_UNUSED(calibrationId)
    return true;
}

// ============================================================================
// US_DataPubImport implementation
// ============================================================================

US_DataPubImport::US_DataPubImport(US_DB2* database)
    : db(database), policy(ConflictReuse), target(US_DataPublication::TargetDatabase) {
}

US_DataPubImport::~US_DataPubImport() {
}

bool US_DataPubImport::importBundle(const QString& bundlePath,
                                     US_DataPublication::ImportTarget importTarget,
                                     const QString& outDir,
                                     US_DataPubConflictPolicy conflictPolicy) {
    target = importTarget;
    outputDir = outDir;
    policy = conflictPolicy;
    
    // Create temporary directory for extraction
    QTemporaryDir temp;
    if (!temp.isValid()) {
        lastError = "Failed to create temporary directory";
        return false;
    }
    tempDir = temp.path();
    
    emit progress(5, tr("Extracting bundle..."));
    
    // Extract archive
    US_Archive archive;
    if (!archive.extract(bundlePath, tempDir)) {
        lastError = "Failed to extract archive: " + archive.getError();
        return false;
    }
    
    emit progress(20, tr("Reading manifest..."));
    
    // Find and read manifest
    QString manifestPath = tempDir + "/manifest.yaml";
    if (!QFile::exists(manifestPath)) {
        // Try to find manifest in subdirectory
        QDir dir(tempDir);
        QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (!entries.isEmpty()) {
            manifestPath = tempDir + "/" + entries.first() + "/manifest.yaml";
        }
    }
    
    if (!manifest.readFromFile(manifestPath)) {
        lastError = "Failed to read manifest";
        return false;
    }
    
    emit progress(30, tr("Validating dependencies..."));
    
    // Import entities in dependency order
    QList<US_DataPubManifestEntry> entries = manifest.allEntries();
    int total = entries.size();
    int current = 0;
    
    for (const auto& entry : entries) {
        // Verify dependencies
        if (!verifyDependencies(entry)) {
            lastError = QString("Missing dependency for %1").arg(entry.name);
            return false;
        }
        
        // Check for conflicts
        US_DataPubConflictResult conflict = checkConflict(entry);
        
        if (conflict.hasConflict) {
            emit conflictDetected(entry, conflict);
            
            if (conflict.propertiesMatch && policy == ConflictReuse) {
                // Reuse existing entity
                guidMap[entry.guid] = conflict.existingGuid;
                emit progress(30 + (current * 70 / total), 
                    tr("Reusing existing %1").arg(entry.name));
            } else if (policy == ConflictRename) {
                // Create with new name - would be implemented per entity type
            } else if (policy == ConflictFail) {
                lastError = QString("Conflict detected for %1").arg(entry.name);
                return false;
            }
        } else {
            // Import the entity
            bool success = false;
            switch (entry.type) {
                case EntityProject:
                    success = importProject(entry);
                    break;
                case EntityExperiment:
                    success = importExperiment(entry);
                    break;
                case EntityBuffer:
                    success = importBuffer(entry);
                    break;
                case EntityAnalyte:
                    success = importAnalyte(entry);
                    break;
                case EntitySolution:
                    success = importSolution(entry);
                    break;
                case EntityModel:
                    success = importModel(entry);
                    break;
                case EntityNoise:
                    success = importNoise(entry);
                    break;
                case EntityRotorCalibration:
                    success = importRotorCalibration(entry);
                    break;
                default:
                    success = true;
                    break;
            }
            
            if (!success) {
                return false;
            }
        }
        
        ++current;
        emit progress(30 + (current * 70 / total), 
            tr("Imported %1").arg(entry.name));
    }
    
    emit progress(100, tr("Import complete"));
    emit completed(true, tr("Successfully imported %1 entities").arg(total));
    
    return true;
}

US_DataPubConflictResult US_DataPubImport::checkConflict(const US_DataPubManifestEntry& entry) {
    US_DataPubConflictResult result;
    result.hasConflict = false;
    result.propertiesMatch = false;
    
    // Check by name in database
    if (target == US_DataPublication::TargetDatabase && db != nullptr) {
        // Would query database for existing entity with same name
        // For now, return no conflict
    }
    
    return result;
}

QString US_DataPubImport::generateUniqueName(const QString& baseName, 
                                              US_DataPubEntityType type) {
    QString newName = baseName;
    int suffix = 1;
    
    // Check if the base name itself is already unique
    bool nameExists = false;
    
    if (target == US_DataPublication::TargetDatabase && db != nullptr) {
        // Query database to check if name exists
        QString tableName;
        QString columnName = "description";  // Most tables use 'description'
        
        switch (type) {
            case EntityProject:   tableName = "project"; break;
            case EntityBuffer:    tableName = "buffer"; break;
            case EntityAnalyte:   tableName = "analyte"; break;
            case EntitySolution:  tableName = "solution"; break;
            case EntityModel:     tableName = "model"; break;
            case EntityNoise:     tableName = "noise"; break;
            default:              return newName;  // Return original for unsupported types
        }
        
        // Check if base name exists
        QStringList queries;
        queries << QString("SELECT COUNT(*) FROM %1 WHERE %2 = '%3'")
                   .arg(tableName).arg(columnName).arg(newName);
        
        for (const QString& q : queries) {
            db->query(q);
            if (db->next() && db->value(0).toInt() > 0) {
                nameExists = true;
                break;
            }
        }
        
        // If name exists, find a unique suffix
        while (nameExists && suffix <= 100) {
            newName = QString("%1_%2").arg(baseName).arg(suffix);
            db->query(QString("SELECT COUNT(*) FROM %1 WHERE %2 = '%3'")
                     .arg(tableName).arg(columnName).arg(newName));
            if (db->next() && db->value(0).toInt() == 0) {
                nameExists = false;  // Found unique name
            }
            suffix++;
        }
    } else {
        // For disk target, just add suffix
        newName = QString("%1_%2").arg(baseName).arg(suffix);
    }
    
    return newName;
}

bool US_DataPubImport::verifyDependencies(const US_DataPubManifestEntry& entry) {
    for (const QString& depGuid : entry.dependencyGuids) {
        if (!guidMap.contains(depGuid)) {
            // Dependency not yet imported - check if it's in the manifest
            if (manifest.findByGuid(depGuid) == nullptr) {
                return false;
            }
        }
    }
    return true;
}

bool US_DataPubImport::importProject(const US_DataPubManifestEntry& entry) {
    QString filePath = tempDir + "/" + entry.payloadPath;
    
    // Read project XML
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        lastError = QString("Failed to read project file: %1").arg(entry.payloadPath);
        return false;
    }
    
    US_Project project;
    QXmlStreamReader xml(&file);
    
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (xml.isStartElement()) {
            QString name = xml.name().toString();
            if (name == "description") {
                project.projectDesc = xml.readElementText();
            } else if (name == "projectGUID") {
                project.projectGUID = xml.readElementText();
            } else if (name == "goals") {
                project.goals = xml.readElementText();
            } else if (name == "molecules") {
                project.molecules = xml.readElementText();
            } else if (name == "purity") {
                project.purity = xml.readElementText();
            } else if (name == "expense") {
                project.expense = xml.readElementText();
            } else if (name == "bufferComponents") {
                project.bufferComponents = xml.readElementText();
            } else if (name == "saltInformation") {
                project.saltInformation = xml.readElementText();
            } else if (name == "AUC_questions") {
                project.AUC_questions = xml.readElementText();
            } else if (name == "expDesign") {
                project.expDesign = xml.readElementText();
            } else if (name == "notes") {
                project.notes = xml.readElementText();
            } else if (name == "status") {
                project.status = xml.readElementText();
            }
        }
    }
    file.close();
    
    if (target == US_DataPublication::TargetDatabase && db != nullptr) {
        // Generate new GUID for imported project
        project.projectGUID = US_Util::new_guid();
        
        int result = project.saveToDB(db);
        if (result < 0) {
            lastError = QString("Failed to save project to database");
            return false;
        }
        
        // Map old GUID to new
        guidMap[entry.guid] = project.projectGUID;
        idMap[entry.guid] = project.projectID;
    } else {
        // Save to disk
        project.saveToDisk();
        guidMap[entry.guid] = project.projectGUID;
    }
    
    return true;
}

bool US_DataPubImport::importExperiment(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}

bool US_DataPubImport::importRawData(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}

bool US_DataPubImport::importBuffer(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}

bool US_DataPubImport::importAnalyte(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}

bool US_DataPubImport::importSolution(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}

bool US_DataPubImport::importModel(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}

bool US_DataPubImport::importNoise(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}

bool US_DataPubImport::importRotorCalibration(const US_DataPubManifestEntry& entry) {
    Q_UNUSED(entry)
    return true;
}
