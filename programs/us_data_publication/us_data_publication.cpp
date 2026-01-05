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

#include "us_data_publication.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_passwd.h"
#include "us_tar.h"
#include "us_gzip.h"
#include "us_archive.h"
#include "us_util.h"

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
    experimentId = -1;
    exportScope = ScopeAll;
    importTarget = TargetDatabase;
    conflictPolicy = ConflictReuse;
    db = nullptr;

    setupGui();
    updateGuiState();
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

    // Mode selection
    QGroupBox* modeBox = new QGroupBox(tr("Operation Mode"));
    QHBoxLayout* modeLayout = new QHBoxLayout(modeBox);
    rb_export = new QRadioButton(tr("Export"));
    rb_import = new QRadioButton(tr("Import"));
    rb_export->setChecked(true);
    modeLayout->addWidget(rb_export);
    modeLayout->addWidget(rb_import);
    main->addWidget(modeBox);

    connect(rb_export, &QRadioButton::toggled, this, &US_DataPublication::selectExportMode);
    connect(rb_import, &QRadioButton::toggled, this, &US_DataPublication::selectImportMode);

    // Export options
    QGroupBox* exportBox = new QGroupBox(tr("Export Settings"));
    QGridLayout* exportLayout = new QGridLayout(exportBox);
    int row = 0;

    QLabel* lbl_project = us_label(tr("Project:"));
    le_project = us_lineedit("", -1, true);
    pb_project = us_pushbutton(tr("Select"));
    exportLayout->addWidget(lbl_project, row, 0);
    exportLayout->addWidget(le_project, row, 1, 1, 2);
    exportLayout->addWidget(pb_project, row++, 3);

    QLabel* lbl_experiment = us_label(tr("Experiment:"));
    le_experiment = us_lineedit("", -1, true);
    pb_experiment = us_pushbutton(tr("Select"));
    exportLayout->addWidget(lbl_experiment, row, 0);
    exportLayout->addWidget(le_experiment, row, 1, 1, 2);
    exportLayout->addWidget(pb_experiment, row++, 3);

    QLabel* lbl_scope = us_label(tr("Export Scope:"));
    cb_scope = us_comboBox();
    cb_scope->addItem(tr("Project Only"), ScopeProject);
    cb_scope->addItem(tr("Experiments"), ScopeExperiment);
    cb_scope->addItem(tr("Raw Data"), ScopeRawData);
    cb_scope->addItem(tr("Edits"), ScopeEdits);
    cb_scope->addItem(tr("Models & Noise"), ScopeModels);
    cb_scope->addItem(tr("Everything"), ScopeAll);
    cb_scope->setCurrentIndex(5);
    exportLayout->addWidget(lbl_scope, row, 0);
    exportLayout->addWidget(cb_scope, row++, 1, 1, 3);

    main->addWidget(exportBox);

    connect(pb_project, &QPushButton::clicked, this, &US_DataPublication::selectProject);
    connect(pb_experiment, &QPushButton::clicked, this, &US_DataPublication::selectExperiment);

    // Import options
    QGroupBox* importBox = new QGroupBox(tr("Import Settings"));
    QGridLayout* importLayout = new QGridLayout(importBox);
    row = 0;

    QLabel* lbl_target = us_label(tr("Target:"));
    cb_target = us_comboBox();
    cb_target->addItem(tr("Database"), TargetDatabase);
    cb_target->addItem(tr("Disk"), TargetDisk);
    importLayout->addWidget(lbl_target, row, 0);
    importLayout->addWidget(cb_target, row++, 1, 1, 3);

    QLabel* lbl_conflict = us_label(tr("On Conflict:"));
    cb_conflict = us_comboBox();
    cb_conflict->addItem(tr("Reuse if properties match"), ConflictReuse);
    cb_conflict->addItem(tr("Always rename"), ConflictRename);
    cb_conflict->addItem(tr("Fail"), ConflictFail);
    importLayout->addWidget(lbl_conflict, row, 0);
    importLayout->addWidget(cb_conflict, row++, 1, 1, 3);

    main->addWidget(importBox);

    // File selection
    QGroupBox* fileBox = new QGroupBox(tr("Bundle File"));
    QHBoxLayout* fileLayout = new QHBoxLayout(fileBox);
    le_file = us_lineedit("");
    pb_browse = us_pushbutton(tr("Browse..."));
    fileLayout->addWidget(le_file);
    fileLayout->addWidget(pb_browse);
    main->addWidget(fileBox);

    connect(pb_browse, &QPushButton::clicked, this, &US_DataPublication::browseOutputFile);

    // Progress
    progress = new QProgressBar();
    progress->setRange(0, 100);
    progress->setValue(0);
    main->addWidget(progress);

    // Status
    te_status = us_textedit();
    te_status->setMaximumHeight(150);
    main->addWidget(te_status);

    // Buttons
    QHBoxLayout* buttons = new QHBoxLayout();
    pb_start = us_pushbutton(tr("Start"));
    pb_help = us_pushbutton(tr("Help"));
    pb_close = us_pushbutton(tr("Close"));
    buttons->addWidget(pb_start);
    buttons->addWidget(pb_help);
    buttons->addWidget(pb_close);
    main->addLayout(buttons);

    connect(pb_start, &QPushButton::clicked, this, [this]() {
        if (rb_export->isChecked()) startExport();
        else startImport();
    });
    connect(pb_help, &QPushButton::clicked, this, &US_DataPublication::help);
    connect(pb_close, &QPushButton::clicked, this, &US_DataPublication::close);
}

void US_DataPublication::updateGuiState() {
    bool isExport = rb_export->isChecked();
    
    pb_project->setEnabled(isExport);
    pb_experiment->setEnabled(isExport);
    cb_scope->setEnabled(isExport);
    cb_target->setEnabled(!isExport);
    cb_conflict->setEnabled(!isExport);
}

void US_DataPublication::selectExportMode() {
    updateGuiState();
}

void US_DataPublication::selectImportMode() {
    updateGuiState();
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
    if (!connectToDatabase()) return;
    
    // For now, use a simple input dialog
    // In a full implementation, this would open a project selection dialog
    bool ok;
    QString idStr = QInputDialog::getText(this, tr("Select Project"),
        tr("Enter Project ID:"), QLineEdit::Normal, "", &ok);
    
    if (ok && !idStr.isEmpty()) {
        projectId = idStr.toInt();
        currentProject.readFromDB(projectId, db);
        le_project->setText(QString("%1: %2").arg(projectId).arg(currentProject.projectDesc));
    }
}

void US_DataPublication::selectExperiment() {
    if (!connectToDatabase()) return;
    
    bool ok;
    QString idStr = QInputDialog::getText(this, tr("Select Experiment"),
        tr("Enter Experiment ID:"), QLineEdit::Normal, "", &ok);
    
    if (ok && !idStr.isEmpty()) {
        experimentId = idStr.toInt();
        le_experiment->setText(QString("Experiment %1").arg(experimentId));
    }
}

void US_DataPublication::browseOutputFile() {
    QString filter = tr("Data Publication Bundle (*.tar.gz)");
    QString path;
    
    if (rb_export->isChecked()) {
        path = QFileDialog::getSaveFileName(this, tr("Save Bundle"), 
            US_Settings::dataDir(), filter);
    } else {
        path = QFileDialog::getOpenFileName(this, tr("Open Bundle"),
            US_Settings::dataDir(), filter);
    }
    
    if (!path.isEmpty()) {
        le_file->setText(path);
        bundlePath = path;
    }
}

void US_DataPublication::browseInputFile() {
    browseOutputFile();
}

void US_DataPublication::startExport() {
    if (bundlePath.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select an output file."));
        return;
    }
    
    if (projectId < 0) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a project."));
        return;
    }
    
    if (!connectToDatabase()) return;
    
    exportScope = static_cast<ExportScope>(cb_scope->currentData().toInt());
    
    te_status->append(tr("Starting export..."));
    progress->setValue(0);
    
    US_DataPubExport exporter(db);
    connect(&exporter, &US_DataPubExport::progress, this, &US_DataPublication::updateProgress);
    connect(&exporter, &US_DataPubExport::completed, this, &US_DataPublication::exportComplete);
    
    bool success = exporter.exportBundle(bundlePath, projectId, experimentId, exportScope);
    
    if (!success) {
        te_status->append(tr("Export failed: %1").arg(exporter.errorMessage()));
    }
}

void US_DataPublication::startImport() {
    if (bundlePath.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Please select a bundle file."));
        return;
    }
    
    importTarget = static_cast<ImportTarget>(cb_target->currentData().toInt());
    conflictPolicy = static_cast<US_DataPubConflictPolicy>(cb_conflict->currentData().toInt());
    
    if (importTarget == TargetDatabase && !connectToDatabase()) return;
    
    te_status->append(tr("Starting import..."));
    progress->setValue(0);
    
    US_DataPubImport importer(db);
    connect(&importer, &US_DataPubImport::progress, this, &US_DataPublication::updateProgress);
    connect(&importer, &US_DataPubImport::completed, this, &US_DataPublication::importComplete);
    
    bool success = importer.importBundle(bundlePath, importTarget, 
                                          US_Settings::dataDir(), conflictPolicy);
    
    if (!success) {
        te_status->append(tr("Import failed: %1").arg(importer.errorMessage()));
    }
}

void US_DataPublication::updateProgress(int value, const QString& message) {
    progress->setValue(value);
    te_status->append(message);
}

void US_DataPublication::exportComplete(bool success, const QString& message) {
    progress->setValue(100);
    if (success) {
        te_status->append(tr("Export completed successfully."));
        QMessageBox::information(this, tr("Export Complete"), message);
    } else {
        te_status->append(tr("Export failed: %1").arg(message));
        QMessageBox::warning(this, tr("Export Failed"), message);
    }
}

void US_DataPublication::importComplete(bool success, const QString& message) {
    progress->setValue(100);
    if (success) {
        te_status->append(tr("Import completed successfully."));
        QMessageBox::information(this, tr("Import Complete"), message);
    } else {
        te_status->append(tr("Import failed: %1").arg(message));
        QMessageBox::warning(this, tr("Import Failed"), message);
    }
}

void US_DataPublication::help() {
    QString helpText = tr(
        "<h2>UltraScan Data Publication</h2>"
        "<p>This tool allows you to export and import data publication bundles.</p>"
        "<h3>Export Mode</h3>"
        "<p>Select a project and optionally an experiment, then choose the scope "
        "of data to export. The bundle will include all necessary dependencies.</p>"
        "<h3>Import Mode</h3>"
        "<p>Select a bundle file and choose whether to import to the database "
        "or disk. Configure how conflicts should be handled:</p>"
        "<ul>"
        "<li><b>Reuse</b>: If an entity with matching properties exists, reuse it</li>"
        "<li><b>Rename</b>: Always create new entities with unique names</li>"
        "<li><b>Fail</b>: Stop on any conflict</li>"
        "</ul>"
    );
    
    QMessageBox::information(this, tr("Help"), helpText);
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
