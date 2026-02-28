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
#include <QSet>

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
#include "us_investigator.h"

// Bundle format version
static const QString BUNDLE_VERSION = "1.0";

//! \brief Helper function to remove surrounding quotes from a string
static QString removeQuotes(const QString& str) {
    QString result = str;
    if (result.startsWith('"')) result = result.mid(1);
    if (result.endsWith('"')) result.chop(1);
    return result;
}

//! \brief Helper function to unescape quotes in a string
static QString unescapeQuotes(const QString& str) {
    QString result = str;
    result.replace("\\\"", "\"");
    return result;
}

//! \brief Convert entity type to string
static QString entityTypeToString(US_DataPubEntityType type) {
    static const char* names[] = {
        "project", "experiment", "rawData", "rotorCalibration",
        "centerpiece", "buffer", "analyte", "solution",
        "edit", "model", "noise", "timestate"
    };
    return (type >= 0 && type < EntityTypeCount) ? names[type] : "unknown";
}

//! \brief Convert string to entity type
static US_DataPubEntityType stringToEntityType(const QString& str) {
    static QMap<QString, US_DataPubEntityType> map;
    if (map.isEmpty()) {
        // Keys must be lowercase to match the .toLower() lookup below
        map["project"]           = EntityProject;
        map["experiment"]        = EntityExperiment;
        map["rawdata"]           = EntityRawData;
        map["rotorcalibration"]  = EntityRotorCalibration;
        map["centerpiece"]       = EntityCenterpiece;
        map["buffer"]            = EntityBuffer;
        map["analyte"]           = EntityAnalyte;
        map["solution"]          = EntitySolution;
        map["edit"]              = EntityEdit;
        map["model"]             = EntityModel;
        map["noise"]             = EntityNoise;
        map["timestate"]         = EntityTimeState;
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
    // Use a local escaped copy to avoid mutating the stored description
    QString escapedDescription = description;
    escapedDescription.replace("\"", "\\\"");
    out << "description: \"" << escapedDescription << "\"\n";
    out << "\n";

    // Group entries by type
    for (int t = 0; t < EntityTypeCount; ++t) {
        US_DataPubEntityType type = static_cast<US_DataPubEntityType>(t);
        QList<US_DataPubManifestEntry> typeEntries = entriesOfType(type);
        
        if (typeEntries.isEmpty()) continue;

        out << entityTypeToString(type) << ":\n";
        for (const auto& entry : typeEntries) {
            // GUID is the primary stable identifier; numeric DB id is optional
            // (disk-only data has no DB id, so id may be 0 or -1)
            out << "  - guid: \"" << entry.guid << "\"\n";
            if (entry.id > 0)
                out << "    id: " << entry.id << "\n";
            // Use a local escaped copy to avoid mutating the stored name
            QString escapedName = entry.name;
            escapedName.replace("\"", "\\\"");
            out << "    name: \"" << escapedName << "\"\n";
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
                description = unescapeQuotes(removeQuotes(trimmed.mid(12).trimmed()));
            } else if (trimmed.endsWith(":")) {
                currentSection = trimmed.left(trimmed.length() - 1);
            }
        } else if (trimmed.startsWith("- guid:")) {
            // Current format: entries start with their GUID (primary stable identifier)
            if (inEntry) {
                entries.append(currentEntry);
            }
            currentEntry = US_DataPubManifestEntry();
            currentEntry.type = stringToEntityType(currentSection);
            currentEntry.guid = removeQuotes(trimmed.mid(7).trimmed());
            inEntry = true;
            inDependencies = false;
        } else if (trimmed.startsWith("- id:")) {
            // Legacy format: entries started with their numeric DB id
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
            } else if (trimmed.startsWith("id:")) {
                // id is now an optional sub-field (emitted only when > 0)
                currentEntry.id = trimmed.mid(3).trimmed().toInt();
            } else if (trimmed.startsWith("name:")) {
                currentEntry.name = unescapeQuotes(removeQuotes(trimmed.mid(5).trimmed()));
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

void US_DataPublication::exportSourceChanged(bool fromDb) {
    useDB = fromDb;
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
    
    // Delete any previously allocated (but not connected) instance to avoid memory leak
    delete db;
    db = nullptr;
    
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
            info.modelID = -1;  // Not needed for export - using GUID instead
            info.description = selectedModels[i].description;
            info.editGUID = selectedModels[i].editGUID;
            info.selected = true;
            
            // Try to load noise information for this model by querying noises for the edit
            // and then filtering by model GUID
            if (fromDB && connectToDatabase() && !info.editGUID.isEmpty()) {
                // First get the editID from the editGUID
                QStringList query;
                query << "get_editedDataID_from_GUID" << info.editGUID;
                db->query(query);
                
                if (db->next()) {
                    QString editID = db->value(0).toString();
                    
                    // Query for noise associated with this edit
                    query.clear();
                    query << "get_noise_desc_by_editID" 
                          << QString::number(US_Settings::us_inv_ID()) 
                          << editID;
                    db->query(query);
                    
                    // Columns from get_noise_desc_by_editID:
                    // 0=noiseID, 1=noiseGUID, 2=editID, 3=unknown, 4=noiseType, 5=modelGUID
                    // (based on usage in gui/us_loadable_noise.cpp)
                    while (db->next()) {
                        QString modelGUID = db->value(5).toString();
                        // Only include noise that belongs to this model
                        if (modelGUID == info.modelGUID) {
                            QString noiseGUID = db->value(1).toString();
                            QString noiseType = db->value(4).toString();
                            
                            if (noiseType.contains("ti", Qt::CaseInsensitive)) {
                                info.noiseGUID_ti = noiseGUID;
                            } else if (noiseType.contains("ri", Qt::CaseInsensitive)) {
                                info.noiseGUID_ri = noiseGUID;
                            }
                        }
                    }
                }
            }
            
            modelList.append(info);
        }
        
        te_statusExport->append(tr("Loaded %1 models.").arg(modelList.size()));
        
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
        // If deselecting raw data, deselect all its edits
        if (!checked) {
            // Deselect all edits for this raw data
            for (int j = 0; j < rawDataList[index].editSelected.size(); ++j) {
                rawDataList[index].editSelected[j] = false;
            }
            // Update the tree UI to reflect deselected edits
            buildDataTree();
            // Check if any models depend on this raw data or its edits
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
    
    // Use the new method that accepts selected data
    bool success = exporter.exportSelectedData(exportBundlePath, currentProject, rawDataList, modelList);
    
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
    
    // Count unique runs with selected raw data (one timestate file per run)
    QSet<QString> uniqueRuns;
    for (const auto& raw : rawDataList) {
        if (raw.selected && !raw.runID.isEmpty())
            uniqueRuns.insert(raw.runID);
    }
    int tsCount = uniqueRuns.size();
    
    QString summary = tr("Summary: %1 raw data, %2 edits, %3 models, %4 timestate(s) selected")
        .arg(rawCount).arg(editCount).arg(modelCount).arg(tsCount);
    
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
    
    if (selectedRunIDs.isEmpty()) {
        te_statusExport->append(tr("No experiments selected."));
        return;
    }
    
    te_statusExport->append(tr("Loading raw data for %1 experiments...").arg(selectedRunIDs.size()));
    
    if (useDB && connectToDatabase()) {
        // Load from database
        for (const QString& runID : selectedRunIDs) {
            // Get experiment ID for this runID
            QStringList query;
            query << "get_experiment_info_by_runID" << runID 
                  << QString::number(US_Settings::us_inv_ID());
            db->query(query);
            
            if (!db->next()) continue;
            
            QString expID = db->value(1).toString();
            
            // Get raw data for this experiment
            query.clear();
            query << "get_raw_desc_by_runID" << QString::number(US_Settings::us_inv_ID()) << runID;
            db->query(query);
            
            while (db->next()) {
                US_DataPubRawDataInfo rawInfo;
                rawInfo.rawID = db->value(0).toInt();
                rawInfo.rawGUID = db->value(7).toString();
                QString filename = db->value(2).toString().replace("\\", "/");
                rawInfo.description = filename.section("/", -1, -1);
                rawInfo.runID = runID;
                rawInfo.expID = expID.toInt();
                rawInfo.selected = true;  // Select by default
                
                rawDataList.append(rawInfo);
            }
        }
        
        // Now load edits for all raw data
        loadEditsForRawData();
        
        te_statusExport->append(tr("Loaded %1 raw data files.").arg(rawDataList.size()));
    } else {
        // Load from disk
        for (const QString& runID : selectedRunIDs) {
            QString aucDir = US_Settings::resultDir() + "/" + runID + "/";
            QDir dir(aucDir);
            
            if (!dir.exists()) continue;
            
            QStringList filters;
            filters << "*.auc";
            QStringList aucFiles = dir.entryList(filters, QDir::Files, QDir::Name);
            
            for (const QString& aucFile : aucFiles) {
                US_DataPubRawDataInfo rawInfo;
                rawInfo.description = aucFile;
                rawInfo.runID = runID;
                rawInfo.selected = true;
                
                // Try to get GUID from XML file
                QString xmlPath = aucDir + aucFile.left(aucFile.length() - 4) + ".xml";
                if (QFile::exists(xmlPath)) {
                    QFile xmlFile(xmlPath);
                    if (xmlFile.open(QIODevice::ReadOnly)) {
                        QXmlStreamReader xml(&xmlFile);
                        while (!xml.atEnd() && !xml.hasError()) {
                            xml.readNext();
                            if (xml.isStartElement() && xml.name() == QLatin1String("rawDataGUID")) {
                                rawInfo.rawGUID = xml.readElementText();
                                break;
                            }
                        }
                        xmlFile.close();
                    }
                }
                
                rawDataList.append(rawInfo);
            }
        }
        
        // Load edits for all raw data
        loadEditsForRawData();
        
        te_statusExport->append(tr("Loaded %1 raw data files from disk.").arg(rawDataList.size()));
    }
    
    updateExportButtonStates();
}

void US_DataPublication::loadEditsForRawData() {
    // Load edit information for the raw data
    
    if (useDB && db != nullptr && db->isConnected()) {
        // Load from database
        for (int i = 0; i < rawDataList.size(); ++i) {
            US_DataPubRawDataInfo& rawInfo = rawDataList[i];
            rawInfo.editIDs.clear();
            rawInfo.editGUIDs.clear();
            rawInfo.editNames.clear();
            rawInfo.editSelected.clear();
            
            QStringList query;
            query << "get_editedDataIDs" << QString::number(rawInfo.rawID);
            db->query(query);
            QStringList editIDs;
            editIDs.clear();
            while (db->next()) {
                editIDs.append(db->value(0).toString());
            }

            for (QString editID: editIDs ) {
                int edit_it = editID.toInt();
                QStringList edit_query;
                edit_query << "get_editedData" << editID;
                db->query(edit_query);
                while (db->next()) {
                    QString editGUID = db->value(1).toString();
                    QString filename = db->value(3).toString().replace("\\", "/");
                    QString editName = filename.section("/", -1, -1);

                    rawInfo.editIDs.append(edit_it);
                    rawInfo.editGUIDs.append(editGUID);
                    rawInfo.editNames.append(editName);
                    rawInfo.editSelected.append(true);  // Select by default
                }

            }
        }
    } else {
        // Load from disk
        for (int i = 0; i < rawDataList.size(); ++i) {
            US_DataPubRawDataInfo& rawInfo = rawDataList[i];
            rawInfo.editIDs.clear();
            rawInfo.editGUIDs.clear();
            rawInfo.editNames.clear();
            rawInfo.editSelected.clear();
            
            QString editDir = US_Settings::resultDir() + "/" + rawInfo.runID + "/";
            QDir dir(editDir);
            
            if (!dir.exists()) continue;
            
            // Look for edit files that match this raw data
            QString rawBase = rawInfo.description;
            if (rawBase.endsWith(".auc")) {
                rawBase = rawBase.left(rawBase.length() - 4);
            }
            
            QStringList filters;
            filters << rawBase + ".*.xml";
            QStringList editFiles = dir.entryList(filters, QDir::Files, QDir::Name);
            
            for (const QString& editFile : editFiles) {
                // Skip the raw data XML file itself
                if (!editFile.contains(".edit")) continue;
                
                rawInfo.editNames.append(editFile);
                rawInfo.editSelected.append(true);
                
                // Try to get GUID from the edit file
                QString editPath = editDir + editFile;
                QFile xmlFile(editPath);
                if (xmlFile.open(QIODevice::ReadOnly)) {
                    QXmlStreamReader xml(&xmlFile);
                    while (!xml.atEnd() && !xml.hasError()) {
                        xml.readNext();
                        if (xml.isStartElement() && xml.name() == QLatin1String("editGUID")) {
                            rawInfo.editGUIDs.append(xml.readElementText());
                            break;
                        }
                    }
                    xmlFile.close();
                }
                
                if (rawInfo.editGUIDs.size() < rawInfo.editNames.size()) {
                    rawInfo.editGUIDs.append(QString());  // Empty GUID if not found
                }
            }
        }
    }
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
            
            // Add edit items - use editNames.size() as the authoritative count
            int editCount = raw.editNames.size();
            for (int j = 0; j < editCount; ++j) {
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
        bool editFound = false;
        int foundRawIdx = -1;
        int foundEditIdx = -1;
        
        // Check if the model's edit is selected
        for (int j = 0; j < rawDataList.size(); ++j) {
            for (int k = 0; k < rawDataList[j].editGUIDs.size(); ++k) {
                if (rawDataList[j].editGUIDs[k] == editGUID) {
                    editFound = true;
                    foundRawIdx = j;
                    foundEditIdx = k;
                    if (rawDataList[j].selected && rawDataList[j].editSelected.value(k, false)) {
                        editSelected = true;
                    }
                    break;
                }
            }
            if (editFound) break;
        }
        
        if (!editSelected) {
            // The model depends on an edit that is not selected
            QString message;
            if (editFound) {
                message = tr("Model '%1' depends on an edit that is not selected.\n"
                            "Do you want to automatically select the required edit?")
                            .arg(modelList[i].description);
            } else {
                message = tr("Model '%1' depends on an edit (GUID: %2) that is not in the loaded data.\n"
                            "The model will be exported without its edit dependency.\n"
                            "Do you want to continue?")
                            .arg(modelList[i].description)
                            .arg(editGUID);
            }
            
            QMessageBox::StandardButton reply = QMessageBox::question(this,
                tr("Dependency Required"),
                message,
                QMessageBox::Yes | QMessageBox::No);
            
            if (reply == QMessageBox::Yes) {
                if (editFound && foundRawIdx >= 0 && foundEditIdx >= 0) {
                    // Select the required edit
                    rawDataList[foundRawIdx].selected = true;
                    // Ensure editSelected list is large enough
                    while (rawDataList[foundRawIdx].editSelected.size() <= foundEditIdx) {
                        rawDataList[foundRawIdx].editSelected.append(false);
                    }
                    rawDataList[foundRawIdx].editSelected[foundEditIdx] = true;
                    buildDataTree();
                }
                // If edit not found, we just continue with the export
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
        out << "  - guid: \"" << currentProject.projectGUID << "\"\n";
        out << "    name: \"" << currentProject.projectDesc << "\"\n\n";
    }
    
    int rawCount = countSelectedRawData();
    int editCount = countSelectedEdits();
    int modelCount = countSelectedModels();
    
    // Count unique runs (one timestate per run)
    QSet<QString> uniqueRuns;
    for (const auto& raw : rawDataList) {
        if (raw.selected && !raw.runID.isEmpty())
            uniqueRuns.insert(raw.runID);
    }
    int tsCount = uniqueRuns.size();
    
    out << "# Selected items summary:\n";
    out << "# Raw Data: " << rawCount << "\n";
    out << "# Edits: " << editCount << "\n";
    out << "# Models: " << modelCount << "\n";
    out << "# Timestates: " << tsCount << "\n\n";
    
    if (rawCount > 0) {
        out << "rawData:\n";
        for (const auto& raw : rawDataList) {
            if (raw.selected) {
                out << "  - guid: \"" << raw.rawGUID << "\"\n";
                if (raw.rawID > 0)
                    out << "    id: " << raw.rawID << "\n";
                out << "    name: \"" << raw.description << "\"\n";
            }
        }
        out << "\n";
    }
    
    if (editCount > 0) {
        out << "edit:\n";
        for (const auto& raw : rawDataList) {
            // Only include edits when their parent raw data is selected
            if (raw.selected) {
                for (int j = 0; j < raw.editSelected.size(); ++j) {
                    if (raw.editSelected[j]) {
                        out << "  - guid: \"" << raw.editGUIDs.value(j) << "\"\n";
                        if (raw.editIDs.value(j, -1) > 0)
                            out << "    id: " << raw.editIDs.value(j) << "\n";
                        out << "    name: \"" << raw.editNames.value(j) << "\"\n";
                    }
                }
            }
        }
        out << "\n";
    }
    
    if (modelCount > 0) {
        out << "model:\n";
        for (const auto& model : modelList) {
            if (model.selected) {
                out << "  - guid: \"" << model.modelGUID << "\"\n";
                if (model.modelID > 0)
                    out << "    id: " << model.modelID << "\n";
                out << "    name: \"" << model.description << "\"\n";
            }
        }
        out << "\n";
    }
    
    if (tsCount > 0) {
        out << "timestate:\n";
        for (const QString& runID : uniqueRuns) {
            // Derive the same deterministic GUID used by exportTimeState()
            QByteArray hash = QCryptographicHash::hash(runID.toUtf8(),
                                                       QCryptographicHash::Md5);
            QString h = QString::fromLatin1(hash.toHex());
            QString tsGuid = QString("%1-%2-%3-%4-%5")
                             .arg(h.left(8)).arg(h.mid(8,4))
                             .arg(h.mid(12,4)).arg(h.mid(16,4))
                             .arg(h.mid(20));
            out << "  - guid: \"" << tsGuid << "\"\n";
            out << "    name: \"" << runID << ".time_state.tmst\"\n";
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
        // Only count edits whose parent raw data is selected
        if (raw.selected) {
            for (bool sel : raw.editSelected) {
                if (sel) ++count;
            }
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
    
    // Create tar.gz archive - compress individual entries so manifest.yaml
    // appears at the archive root (not under a random temp dir name)
    US_Archive archive;
    QStringList sources;
    QDir tdir(tempDir.path());
    QStringList tdirEntries = tdir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QString& tdirEntry : tdirEntries) {
        sources << tdir.absoluteFilePath(tdirEntry);
    }
    
    QString archivePath = bundlePath;
    if (!archive.compress(sources, archivePath)) {
        lastError = "Failed to create archive: " + archive.getError();
        return false;
    }
    
    emit progress(100, tr("Export complete"));
    emit completed(true, tr("Bundle created successfully at %1").arg(bundlePath));
    
    return true;
}

bool US_DataPubExport::exportSelectedData(const QString& bundlePath,
                                          const US_Project& project,
                                          const QList<US_DataPubRawDataInfo>& rawDataList,
                                          const QList<US_DataPubModelInfo>& modelList) {
    // Create temporary directory
    QTemporaryDir tempDirObj;
    if (!tempDirObj.isValid()) {
        lastError = "Failed to create temporary directory";
        return false;
    }
    this->tempDir = tempDirObj.path();
    
    manifest.clear();
    exportedSolutions.clear();
    exportedTimestates.clear();
    exportedNoises.clear();
    manifest.description = QString("Data publication bundle created %1")
        .arg(QDateTime::currentDateTime().toString());
    
    emit progress(5, tr("Creating payload directories..."));
    
    // Create payload directories
    for (int t = 0; t < EntityTypeCount; ++t) {
        QString typeName = entityTypeToString(static_cast<US_DataPubEntityType>(t));
        QDir(tempDir).mkdir(typeName);
    }
    
    int totalItems = 0;
    int processedItems = 0;
    
    // Count total items to process
    for (const auto& raw : rawDataList) {
        if (raw.selected) totalItems++;
        for (bool sel : raw.editSelected) {
            if (sel) totalItems++;
        }
    }
    for (const auto& model : modelList) {
        if (model.selected) totalItems++;
    }
    
    // Export project if available
    if (project.projectID > 0) {
        emit progress(10, tr("Exporting project..."));
        if (!exportProject(project)) {
            return false;
        }
    }
    
    // Export raw data, edits, and timestate
    emit progress(15, tr("Exporting raw data and edits..."));
    for (const auto& raw : rawDataList) {
        if (raw.selected) {
            if (!exportRawDataFile(raw)) {
                return false;
            }
            processedItems++;
            int pct = 15 + (processedItems * 50 / (totalItems > 0 ? totalItems : 1));
            emit progress(pct, tr("Exported raw data: %1").arg(raw.description));
            
            // Export timestate once per unique run (it covers all raw data in the run)
            if (!raw.runID.isEmpty() && !exportedTimestates.contains(raw.runID)) {
                exportTimeState(raw.runID, raw.expID);
                exportedTimestates.insert(raw.runID);
            }
            
            // Export selected edits only when their parent raw data is selected
            for (int j = 0; j < raw.editSelected.size(); ++j) {
                if (raw.editSelected[j]) {
                    if (!exportEditData(raw, j)) {
                        return false;
                    }
                    processedItems++;
                    int epct = 15 + (processedItems * 50 / (totalItems > 0 ? totalItems : 1));
                    emit progress(epct, tr("Exported edit: %1").arg(raw.editNames.value(j)));
                }
            }
        }
    }
    
    // Export models and their noise
    emit progress(65, tr("Exporting models..."));
    for (const auto& model : modelList) {
        if (model.selected) {
            if (!exportModel(model)) {
                return false;
            }
            processedItems++;
            int pct = 65 + (processedItems * 20 / (totalItems > 0 ? totalItems : 1));
            emit progress(pct, tr("Exported model: %1").arg(model.description));
            
            // Export associated noise
            if (!model.noiseGUID_ti.isEmpty()) {
                exportNoise(model.noiseGUID_ti);
            }
            if (!model.noiseGUID_ri.isEmpty()) {
                exportNoise(model.noiseGUID_ri);
            }
        }
    }
    
    emit progress(85, tr("Writing manifest..."));
    
    // Write manifest
    QString manifestPath = tempDir + "/manifest.yaml";
    if (!manifest.writeToFile(manifestPath)) {
        lastError = "Failed to write manifest";
        return false;
    }
    
    emit progress(90, tr("Creating archive..."));
    
    // Create tar.gz archive - compress individual entries so manifest.yaml
    // appears at the archive root (not under a random temp dir name)
    US_Archive archive;
    QStringList sources;
    QDir td(tempDir);
    QStringList tdEntries = td.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QString& tdEntry : tdEntries) {
        sources << td.absoluteFilePath(tdEntry);
    }
    
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

bool US_DataPubExport::exportRawData(const QString& rawGuid, int rawID) {
    Q_UNUSED(rawGuid)
    Q_UNUSED(rawID)
    return true;
}

bool US_DataPubExport::exportRawDataFile(const US_DataPubRawDataInfo& rawInfo) {
    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.id = rawInfo.rawID;
    entry.guid = rawInfo.rawGUID;
    entry.name = rawInfo.description;
    entry.type = EntityRawData;

    // Build payload path - strip .auc extension if already present in description
    QString baseName = rawInfo.rawGUID.isEmpty() ? rawInfo.description : rawInfo.rawGUID;
    if (baseName.endsWith(".auc", Qt::CaseInsensitive)) {
        baseName.chop(4);
    }
    entry.payloadPath = QString("rawData/%1.auc").arg(baseName);
    
    // Compute property hash
    QVariantMap props;
    props["description"] = rawInfo.description;
    props["runID"] = rawInfo.runID;
    entry.propertyHash = computePropertyHash(props);
    
    // Copy the AUC file
    QString srcPath = US_Settings::resultDir() + "/" + rawInfo.runID + "/" + rawInfo.description;
    
    QString destPath = tempDir + "/" + entry.payloadPath;
    QDir().mkpath(QFileInfo(destPath).path());
    
    if (!QFile::exists(srcPath)) {
        lastError = QString("Raw data file not found: %1").arg(srcPath);
        return false;
    }
    
    if (!QFile::copy(srcPath, destPath)) {
        lastError = QString("Failed to copy raw data file from %1 to %2").arg(srcPath, destPath);
        return false;
    }
    
    // Also export the solution for this raw data if we have it
    // Solutions are associated with experiments, so query through expID
    if (db != nullptr && db->isConnected() && rawInfo.expID > 0) {
        QStringList query;
        // Query for solutions associated with this experiment
        query << "get_solutionIDs" << QString::number(rawInfo.expID);
        db->query(query);
        
        // Export all solutions for this experiment (typically there's one per channel)
        while (db->next()) {
            int solutionID = db->value(0).toInt();
            if (solutionID > 0) {
                exportSolutionByID(solutionID);
            }
        }
    }
    
    manifest.addEntry(entry);
    return true;
}

bool US_DataPubExport::exportEditData(const US_DataPubRawDataInfo& rawInfo, int editIndex) {
    if (editIndex < 0 || editIndex >= rawInfo.editNames.size()) {
        return false;
    }

    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.id = rawInfo.editIDs.value(editIndex, -1);
    entry.guid = rawInfo.editGUIDs.value(editIndex);
    entry.name = rawInfo.editNames.value(editIndex);
    entry.type = EntityEdit;

    // Build payload path - strip .xml extension if already present in name
    QString baseName = entry.guid.isEmpty() ? entry.name : entry.guid;
    if (baseName.endsWith(".xml", Qt::CaseInsensitive)) {
        baseName.chop(4);
    }
    entry.payloadPath = QString("edit/%1.xml").arg(baseName);
    
    // Add dependency on raw data
    if (!rawInfo.rawGUID.isEmpty()) {
        entry.dependencyGuids.append(rawInfo.rawGUID);
    }
    
    // Compute property hash
    QVariantMap props;
    props["name"] = entry.name;
    props["rawGUID"] = rawInfo.rawGUID;
    entry.propertyHash = computePropertyHash(props);
    
    // Copy the edit file
    QString srcPath = US_Settings::resultDir() + "/" + rawInfo.runID + "/" + entry.name;
    QString destPath = tempDir + "/" + entry.payloadPath;
    QDir().mkpath(QFileInfo(destPath).path());
    
    if (!QFile::exists(srcPath)) {
        lastError = QString("Edit file not found: %1").arg(srcPath);
        return false;
    }
    
    if (!QFile::copy(srcPath, destPath)) {
        lastError = QString("Failed to copy edit file from %1 to %2").arg(srcPath, destPath);
        return false;
    }
    
    manifest.addEntry(entry);
    return true;
}

bool US_DataPubExport::exportBuffer(const US_Buffer& buffer) {
    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.id = buffer.bufferID.toInt();
    entry.guid = buffer.GUID;
    entry.name = buffer.description;
    entry.type = EntityBuffer;
    entry.payloadPath = QString("buffer/%1.xml").arg(buffer.GUID);
    
    // Compute property hash
    QVariantMap props;
    props["description"] = buffer.description;
    props["pH"] = buffer.pH;
    props["density"] = buffer.density;
    props["viscosity"] = buffer.viscosity;
    entry.propertyHash = computePropertyHash(props);
    
    // Write buffer XML
    QString filePath = tempDir + "/" + entry.payloadPath;
    QDir().mkpath(QFileInfo(filePath).path());
    
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        lastError = "Failed to write buffer file";
        return false;
    }
    
    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("Buffer");
    xml.writeTextElement("bufferID", buffer.bufferID);
    xml.writeTextElement("GUID", buffer.GUID);
    xml.writeTextElement("description", buffer.description);
    xml.writeTextElement("pH", QString::number(buffer.pH));
    xml.writeTextElement("density", QString::number(buffer.density));
    xml.writeTextElement("viscosity", QString::number(buffer.viscosity));
    xml.writeTextElement("compressibility", QString::number(buffer.compressibility));
    xml.writeEndElement();
    xml.writeEndDocument();
    file.close();
    
    manifest.addEntry(entry);
    return true;
}

bool US_DataPubExport::exportAnalyte(const US_Analyte& analyte) {
    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.id = -1;  // Analytes use GUID as primary identifier in the DB
    entry.guid = analyte.analyteGUID;
    entry.name = analyte.description;
    entry.type = EntityAnalyte;
    entry.payloadPath = QString("analyte/%1.xml").arg(analyte.analyteGUID);
    
    // Compute property hash
    QVariantMap props;
    props["description"] = analyte.description;
    props["mw"] = analyte.mw;
    props["vbar20"] = analyte.vbar20;
    entry.propertyHash = computePropertyHash(props);
    
    // Write analyte XML
    QString filePath = tempDir + "/" + entry.payloadPath;
    QDir().mkpath(QFileInfo(filePath).path());
    
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        lastError = "Failed to write analyte file";
        return false;
    }
    
    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("Analyte");
    xml.writeTextElement("analyteGUID", analyte.analyteGUID);
    xml.writeTextElement("description", analyte.description);
    xml.writeTextElement("mw", QString::number(analyte.mw));
    xml.writeTextElement("vbar20", QString::number(analyte.vbar20));
    xml.writeTextElement("type", QString::number(analyte.type));
    xml.writeEndElement();
    xml.writeEndDocument();
    file.close();
    
    manifest.addEntry(entry);
    return true;
}

bool US_DataPubExport::exportSolution(const US_Solution& solution) {
    // Check if already exported
    if (exportedSolutions.contains(solution.solutionGUID)) {
        return true;
    }
    exportedSolutions.insert(solution.solutionGUID);
    
    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.id = solution.solutionID;
    entry.guid = solution.solutionGUID;
    entry.name = solution.solutionDesc;
    entry.type = EntitySolution;
    entry.payloadPath = QString("solution/%1.xml").arg(solution.solutionGUID);
    
    // Add dependency on buffer
    if (!solution.buffer.GUID.isEmpty()) {
        entry.dependencyGuids.append(solution.buffer.GUID);
    }
    
    // Compute property hash
    QVariantMap props;
    props["description"] = solution.solutionDesc;
    props["bufferGUID"] = solution.buffer.GUID;
    entry.propertyHash = computePropertyHash(props);
    
    // Write solution XML
    QString filePath = tempDir + "/" + entry.payloadPath;
    QDir().mkpath(QFileInfo(filePath).path());
    
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        lastError = "Failed to write solution file";
        return false;
    }
    
    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("Solution");
    xml.writeTextElement("solutionID", QString::number(solution.solutionID));
    xml.writeTextElement("solutionGUID", solution.solutionGUID);
    xml.writeTextElement("description", solution.solutionDesc);
    xml.writeTextElement("bufferGUID", solution.buffer.GUID);
    xml.writeTextElement("commonVbar20", QString::number(solution.commonVbar20));
    xml.writeTextElement("storageTemp", QString::number(solution.storageTemp));
    xml.writeEndElement();
    xml.writeEndDocument();
    file.close();
    
    // Export the buffer
    if (solution.buffer.bufferID > 0 || !solution.buffer.GUID.isEmpty()) {
        exportBuffer(solution.buffer);
    }
    
    // Export analytes
    for (const auto& analyte : solution.analyteInfo) {
        exportAnalyte(analyte.analyte);
    }
    
    manifest.addEntry(entry);
    return true;
}

bool US_DataPubExport::exportSolutionByID(int solutionID) {
    if (solutionID <= 0 || db == nullptr) {
        return false;
    }
    
    US_Solution solution;
    int status = solution.readFromDB(solutionID, db);
    if (status == US_DB2::OK) {
        return exportSolution(solution);
    }
    return false;
}

bool US_DataPubExport::exportModel(const US_DataPubModelInfo& modelInfo) {
    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.id = modelInfo.modelID;
    entry.guid = modelInfo.modelGUID;
    entry.name = modelInfo.description;
    entry.type = EntityModel;
    entry.payloadPath = QString("model/%1.xml").arg(modelInfo.modelGUID);

    // Add dependency on edit
    if (!modelInfo.editGUID.isEmpty()) {
        entry.dependencyGuids.append(modelInfo.editGUID);
    }

    // Compute property hash
    QVariantMap props;
    props["description"] = modelInfo.description;
    props["editGUID"] = modelInfo.editGUID;
    entry.propertyHash = computePropertyHash(props);

    // Load and export the model
    bool loadSuccess = false;
    if (db != nullptr && db->isConnected()) {
        US_Model model;
        int status = model.load(true, modelInfo.modelGUID, db);
        if (status == US_DB2::OK) {
            QString filePath = tempDir + "/" + entry.payloadPath;
            QDir().mkpath(QFileInfo(filePath).path());
            if (model.write(filePath)) {
                loadSuccess = true;
            } else {
                lastError = QString("Failed to write model file: %1").arg(filePath);
                return false;
            }
        } else {
            lastError = QString("Failed to load model from database: %1 (status: %2)")
                .arg(modelInfo.modelGUID).arg(status);
            return false;
        }
    } else {
        // From local disk - use US_Model::load(false, guid, nullptr) which scans M*.xml files
        US_Model model;
        int status = model.load(false, modelInfo.modelGUID, nullptr);
        if (status == US_DB2::OK) {
            QString destPath = tempDir + "/" + entry.payloadPath;
            QDir().mkpath(QFileInfo(destPath).path());
            if (model.write(destPath)) {
                loadSuccess = true;
            } else {
                lastError = QString("Failed to write model file: %1").arg(destPath);
                return false;
            }
        } else {
            lastError = QString("Failed to load model from disk: %1 (status: %2)")
                .arg(modelInfo.modelGUID).arg(status);
            return false;
        }
    }

    if (!loadSuccess) {
        lastError = QString("Failed to export model: %1").arg(modelInfo.modelGUID);
        return false;
    }

    manifest.addEntry(entry);
    return true;
}

bool US_DataPubExport::exportNoise(const QString& noiseGuid) {
    if (noiseGuid.isEmpty()) {
        return true;  // Empty GUID is not an error - just skip
    }

    // Skip if already exported
    if (exportedNoises.contains(noiseGuid)) {
        return true;
    }

    // Create manifest entry
    US_DataPubManifestEntry entry;
    entry.guid = noiseGuid;
    entry.type = EntityNoise;
    entry.payloadPath = QString("noise/%1.xml").arg(noiseGuid);

    // Load and export the noise
    bool loadSuccess = false;
    if (db != nullptr && db->isConnected()) {
        US_Noise noise;
        int status = noise.load(noiseGuid, db);
        if (status == US_DB2::OK) {
            entry.id = 0;
            entry.name = noise.description;

            // Add dependency on model
            if (!noise.modelGUID.isEmpty()) {
                entry.dependencyGuids.append(noise.modelGUID);
            }

            // Compute property hash
            QVariantMap props;
            props["description"] = noise.description;
            props["modelGUID"] = noise.modelGUID;
            props["type"] = static_cast<int>(noise.type);
            entry.propertyHash = computePropertyHash(props);

            QString filePath = tempDir + "/" + entry.payloadPath;
            QDir().mkpath(QFileInfo(filePath).path());
            if (noise.write(filePath)) {
                loadSuccess = true;
            } else {
                lastError = QString("Failed to write noise file: %1").arg(filePath);
                return false;
            }
        } else {
            lastError = QString("Failed to load noise from database: %1 (status: %2)")
                .arg(noiseGuid).arg(status);
            return false;
        }
    } else {
        // From local disk - load noise from disk
        US_Noise noise;
        int status = noise.load(false, noiseGuid, nullptr);
        if (status == US_DB2::OK) {
            entry.id = -1;  // No DB ID for disk-only noise
            entry.name = noise.description;

            // Add dependency on model
            if (!noise.modelGUID.isEmpty()) {
                entry.dependencyGuids.append(noise.modelGUID);
            }

            // Compute property hash
            QVariantMap props;
            props["description"] = noise.description;
            props["modelGUID"] = noise.modelGUID;
            props["type"] = static_cast<int>(noise.type);
            entry.propertyHash = computePropertyHash(props);

            QString destPath = tempDir + "/" + entry.payloadPath;
            QDir().mkpath(QFileInfo(destPath).path());
            if (noise.write(destPath)) {
                loadSuccess = true;
            } else {
                lastError = QString("Failed to write noise file: %1").arg(destPath);
                return false;
            }
        } else {
            lastError = QString("Failed to load noise from disk: %1 (status: %2)")
                .arg(noiseGuid).arg(status);
            return false;
        }
    }

    if (!loadSuccess) {
        lastError = QString("Failed to export noise: %1").arg(noiseGuid);
        return false;
    }

    manifest.addEntry(entry);
    exportedNoises.insert(noiseGuid);
    return true;
}

bool US_DataPubExport::exportRotorCalibration(int calibrationId) {
    Q_UNUSED(calibrationId)
    return true;
}

bool US_DataPubExport::exportTimeState(const QString& runID, int expID) {
    if (runID.isEmpty()) {
        return false;
    }

    // Payload: two files — the binary .tmst and its XML definitions sidecar
    QString tmstFilename = runID + ".time_state.tmst";
    QString xmlFilename  = runID + ".time_state.xml";

    US_DataPubManifestEntry entry;
    entry.type        = EntityTimeState;
    entry.name        = tmstFilename;     // human-readable: "<runID>.time_state.tmst"
    // runID is the experiment name (not a GUID). Derive a deterministic UUID-like
    // identifier from the runID via MD5 so the GUID is stable and reproducible.
    {
        QByteArray hash = QCryptographicHash::hash(runID.toUtf8(),
                                                   QCryptographicHash::Md5);
        QString h = QString::fromLatin1(hash.toHex());
        entry.guid = QString("%1-%2-%3-%4-%5")
                     .arg(h.left(8))
                     .arg(h.mid(8,  4))
                     .arg(h.mid(12, 4))
                     .arg(h.mid(16, 4))
                     .arg(h.mid(20));
    }
    entry.payloadPath = QString("timestate/%1").arg(tmstFilename);

    QString destTmst = tempDir + "/" + entry.payloadPath;
    QString destXml  = tempDir + "/timestate/" + xmlFilename;
    QDir().mkpath(QFileInfo(destTmst).path());

    bool gotFile = false;

    if (db != nullptr && db->isConnected() && expID > 0) {
        // Fetch timestate from DB
        int    tmstID = 0;
        QString xdefs;
        int status = US_TimeState::dbExamine(db, &tmstID, &expID, nullptr,
                                              &xdefs, nullptr, nullptr);
        if (status == US_DB2::OK && tmstID > 0) {
            status = US_TimeState::dbDownload(db, tmstID, destTmst);
            if (status == US_DB2::OK) {
                // Write sibling XML definitions file
                if (!xdefs.isEmpty()) {
                    QFile xmlFile(destXml);
                    if (xmlFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        QTextStream ts(&xmlFile);
                        ts << xdefs;
                    }
                }
                gotFile = true;
            }
        }
    }

    if (!gotFile) {
        // Fall back to local disk copy
        QString srcTmst = US_Settings::resultDir() + "/" + runID + "/" + tmstFilename;
        QString srcXml  = US_Settings::resultDir() + "/" + runID + "/" + xmlFilename;
        if (QFile::exists(srcTmst)) {
            if (QFile::copy(srcTmst, destTmst)) {
                gotFile = true;
                if (QFile::exists(srcXml)) {
                    QFile::copy(srcXml, destXml);
                }
            }
        }
    }

    if (!gotFile) {
        // Timestate is not mandatory for all runs; log but do not fail the export
        return true;
    }

    // Compute property hash from runID so the importer can detect duplicates
    QVariantMap props;
    props["runID"] = runID;
    entry.propertyHash = computePropertyHash(props);

    manifest.addEntry(entry);
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
                // Continue to next entry since we're reusing
                continue;
            } else if (policy == ConflictRename) {
                // Treat unimplemented rename policy as an error to avoid inconsistent state
                lastError = QString("Conflict detected for %1 (rename policy not implemented)")
                                .arg(entry.name);
                return false;
            } else if (policy == ConflictFail) {
                lastError = QString("Conflict detected for %1").arg(entry.name);
                return false;
            }
        }

        // Import the entity (only if no conflict or conflict wasn't handled by reuse)
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
    
    if (target == US_DataPublication::TargetDatabase && db != nullptr) {
        // Use stored procedures to fetch existing names (avoids SQL injection risk
        // that comes with embedding user-supplied strings directly into SQL).
        QString invID = QString::number(US_Settings::us_inv_ID());
        QStringList existingNames;
        QStringList query;
        
        // Each type has a stored procedure that lists descriptions
        switch (type) {
            case EntityBuffer: {
                query << "get_buffer_desc" << invID;
                db->query(query);
                while (db->next())
                    existingNames << db->value(1).toString();  // description column
                break;
            }
            case EntityModel: {
                query << "get_model_desc" << invID;
                db->query(query);
                while (db->next())
                    existingNames << db->value(2).toString();  // description column
                break;
            }
            case EntityNoise: {
                query << "get_noise_desc" << invID;
                db->query(query);
                while (db->next())
                    existingNames << db->value(1).toString();  // description column
                break;
            }
            default:
                // For types without a list stored procedure, just add a suffix
                return QString("%1_%2").arg(baseName).arg(suffix);
        }
        
        // Check if base name exists, then increment suffix until unique
        while (existingNames.contains(newName) && suffix <= 100) {
            newName = QString("%1_%2").arg(baseName).arg(suffix++);
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
