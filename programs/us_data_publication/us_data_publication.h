//! \file us_data_publication.h
#ifndef US_DATA_PUBLICATION_H
#define US_DATA_PUBLICATION_H

#include <QtCore>
#include <QtWidgets>
#include <QWizard>
#include <QWizardPage>
#include <QProgressBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTabWidget>

#include "us_widgets.h"
#include "us_project.h"
#include "us_experiment.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_solution.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_rotor.h"
#include "us_dataIO.h"
#include "us_db2.h"
#include "us_help.h"

// Forward declarations
class US_DataPubExport;
class US_DataPubImport;

//! \brief Entity types supported in data publication bundles
enum US_DataPubEntityType {
    EntityProject = 0,
    EntityExperiment,
    EntityRawData,
    EntityRotorCalibration,
    EntityCenterpiece,
    EntityBuffer,
    EntityAnalyte,
    EntitySolution,
    EntityEdit,
    EntityModel,
    EntityNoise,
    EntityTypeCount
};

//! \brief Manifest entry for a single entity in the bundle
struct US_DataPubManifestEntry {
    int                  id;              //!< Local database ID
    QString              guid;            //!< Global unique identifier
    QString              name;            //!< Entity name/description
    US_DataPubEntityType type;            //!< Entity type
    QString              propertyHash;    //!< Hash of properties for comparison
    QString              payloadPath;     //!< Path within bundle
    QStringList          dependencyGuids; //!< GUIDs of dependencies
};

//! \brief Manifest for a data publication bundle
class US_DataPubManifest {
public:
    US_DataPubManifest();
    
    //! \brief Clear all entries
    void clear();
    
    //! \brief Add an entry to the manifest
    void addEntry(const US_DataPubManifestEntry& entry);
    
    //! \brief Get all entries of a specific type
    QList<US_DataPubManifestEntry> entriesOfType(US_DataPubEntityType type) const;
    
    //! \brief Get entry by GUID
    US_DataPubManifestEntry* findByGuid(const QString& guid);
    
    //! \brief Write manifest to YAML file
    bool writeToFile(const QString& filepath);
    
    //! \brief Read manifest from YAML file
    bool readFromFile(const QString& filepath);
    
    //! \brief Get all entries in dependency order
    QList<US_DataPubManifestEntry> allEntries() const;
    
    QString bundleVersion;    //!< Bundle format version
    QString createdAt;        //!< Creation timestamp
    QString description;      //!< Bundle description
    
private:
    QList<US_DataPubManifestEntry> entries;
};

//! \brief Conflict resolution policy
enum US_DataPubConflictPolicy {
    ConflictReuse,   //!< Reuse existing entity if properties match
    ConflictRename,  //!< Force rename with suffix
    ConflictFail     //!< Fail on any conflict
};

//! \brief Result of a conflict check
struct US_DataPubConflictResult {
    bool hasConflict;
    bool propertiesMatch;
    QString existingGuid;
    QString suggestedName;
};

//! \brief Information about a raw data entry for selection
struct US_DataPubRawDataInfo {
    QString runID;           //!< Run identifier
    QString rawGUID;         //!< Raw data GUID
    QString description;     //!< Description
    int     rawID;           //!< Database ID
    int     expID;           //!< Experiment ID
    bool    selected;        //!< Selection state
    QList<int> editIDs;      //!< Associated edit IDs
    QStringList editGUIDs;   //!< Associated edit GUIDs
    QStringList editNames;   //!< Associated edit names
    QList<bool> editSelected; //!< Edit selection states
};

//! \brief Information about a model for selection
struct US_DataPubModelInfo {
    QString modelGUID;       //!< Model GUID
    QString description;     //!< Description
    int     modelID;         //!< Database ID
    QString editGUID;        //!< Parent edit GUID
    QString rawGUID;         //!< Parent raw data GUID
    bool    selected;        //!< Selection state
    QString noiseGUID_ti;    //!< TI noise GUID (if any)
    QString noiseGUID_ri;    //!< RI noise GUID (if any)
};

//! \brief Main Data Publication class - handles both CLI and GUI operations
class US_DataPublication : public US_Widgets {
    Q_OBJECT

public:
    US_DataPublication();
    ~US_DataPublication();

    //! \brief Export mode scope options
    enum ExportScope {
        ScopeProject,
        ScopeExperiment,
        ScopeRawData,
        ScopeEdits,
        ScopeModels,
        ScopeAll
    };

    //! \brief Import target options
    enum ImportTarget {
        TargetDatabase,
        TargetDisk
    };

    //! \brief Run CLI mode with given arguments
    static int runCli(int argc, char* argv[]);

private slots:
    // Export tab slots
    void exportSourceChanged(bool db);
    void selectProject();
    void projectSelected(US_Project& project);
    void selectExperiments();
    void selectModels();
    void selectAllRawData();
    void deselectAllRawData();
    void selectLatestEdits();
    void selectAllEdits();
    void deselectAllEdits();
    void dataTreeItemChanged(QTreeWidgetItem* item, int column);
    void modelTreeItemChanged(QTreeWidgetItem* item, int column);
    void browseExportFile();
    void startExport();
    void showManifestPreview();
    
    // Import tab slots
    void importSourceChanged(bool db);
    void browseImportFile();
    void startImport();
    
    // Common slots
    void updateProgress(int value, const QString& message);
    void exportComplete(bool success, const QString& message);
    void importComplete(bool success, const QString& message);
    void updateSummary();
    void help();

private:
    // Main tab widget
    QTabWidget*    tabWidget;
    
    // Export tab GUI elements
    QWidget*       exportTab;
    US_Disk_DB_Controls* exportDiskDB;
    QPushButton*   pb_selectProject;
    QPushButton*   pb_selectExperiments;
    QPushButton*   pb_selectModels;
    QPushButton*   pb_selectAllRaw;
    QPushButton*   pb_deselectAllRaw;
    QPushButton*   pb_selectLatestEdits;
    QPushButton*   pb_selectAllEdits;
    QPushButton*   pb_deselectAllEdits;
    QPushButton*   pb_browseExport;
    QPushButton*   pb_startExport;
    QPushButton*   pb_manifestPreview;
    QLineEdit*     le_project;
    QLineEdit*     le_experiments;
    QLineEdit*     le_exportFile;
    QTreeWidget*   tw_dataTree;       //!< Tree: experiments->rawData->edits
    QTreeWidget*   tw_modelTree;      //!< Tree: experiments->rawData->edits->models
    QLabel*        lb_summaryExport;
    QProgressBar*  progressExport;
    QTextEdit*     te_statusExport;
    
    // Import tab GUI elements
    QWidget*       importTab;
    US_Disk_DB_Controls* importDiskDB;
    QPushButton*   pb_browseImport;
    QPushButton*   pb_startImport;
    QLineEdit*     le_importFile;
    QComboBox*     cb_conflict;
    QTreeWidget*   tw_importPreview;
    QLabel*        lb_summaryImport;
    QProgressBar*  progressImport;
    QTextEdit*     te_statusImport;
    
    // Common buttons
    QPushButton*   pb_help;
    QPushButton*   pb_close;
    
    // Export data
    US_Project     currentProject;
    int            projectId;
    QStringList    selectedRunIDs;
    QList<int>     selectedExpIDs;
    QString        exportBundlePath;
    QList<US_DataPubRawDataInfo> rawDataList;
    QList<US_DataPubModelInfo>   modelList;
    
    // Import data
    QString        importBundlePath;
    US_DataPubConflictPolicy conflictPolicy;
    
    // Database connection
    US_DB2*        db;
    bool           useDB;
    
    // Help
    US_Help        showHelp;
    
    // Setup functions
    void setupGui();
    void setupExportTab();
    void setupImportTab();
    void buildDataTree();
    void buildModelTree();
    void updateExportButtonStates();
    void updateImportButtonStates();
    bool connectToDatabase();
    
    // Data loading helpers
    void loadRawDataForExperiments();
    void loadEditsForRawData();
    void checkModelDependencies();
    
    // Summary/manifest generation
    QString generateManifestPreview();
    int countSelectedRawData();
    int countSelectedEdits();
    int countSelectedModels();
    
    // Static CLI helpers
    static void printUsage();
    static int  doExport(const QString& bundlePath, int projectId, int experimentId,
                         ExportScope scope, bool verbose, bool dryRun);
    static int  doImport(const QString& bundlePath, ImportTarget target,
                         const QString& outputDir, US_DataPubConflictPolicy policy,
                         bool verbose, bool dryRun);
};

//! \brief Export handler class
class US_DataPubExport : public QObject {
    Q_OBJECT

public:
    US_DataPubExport(US_DB2* db = nullptr);
    ~US_DataPubExport();

    //! \brief Export data to a bundle
    bool exportBundle(const QString& bundlePath, int projectId = -1, 
                      int experimentId = -1, 
                      US_DataPublication::ExportScope scope = US_DataPublication::ScopeAll);

    //! \brief Get error message
    QString errorMessage() const { return lastError; }

signals:
    void progress(int percent, const QString& message);
    void completed(bool success, const QString& message);

private:
    US_DB2* db;
    QString lastError;
    QString tempDir;
    US_DataPubManifest manifest;

    // Export helpers
    bool exportProject(const US_Project& project);
    bool exportExperiment(int expId);
    bool exportRawData(const QString& rawGuid);
    bool exportBuffer(const US_Buffer& buffer);
    bool exportAnalyte(const US_Analyte& analyte);
    bool exportSolution(const US_Solution& solution);
    bool exportModel(const QString& modelGuid);
    bool exportNoise(const QString& noiseGuid);
    bool exportRotorCalibration(int calibrationId);
    
    QString computePropertyHash(const QVariantMap& properties);
    void gatherDependencies(int projectId, int experimentId, 
                            US_DataPublication::ExportScope scope);
};

//! \brief Import handler class  
class US_DataPubImport : public QObject {
    Q_OBJECT

public:
    US_DataPubImport(US_DB2* db = nullptr);
    ~US_DataPubImport();

    //! \brief Import bundle to database or disk
    bool importBundle(const QString& bundlePath, 
                      US_DataPublication::ImportTarget target,
                      const QString& outputDir = QString(),
                      US_DataPubConflictPolicy policy = ConflictReuse);

    //! \brief Get error message
    QString errorMessage() const { return lastError; }

signals:
    void progress(int percent, const QString& message);
    void completed(bool success, const QString& message);
    void conflictDetected(const US_DataPubManifestEntry& entry, 
                          const US_DataPubConflictResult& result);

private:
    US_DB2* db;
    QString lastError;
    QString tempDir;
    US_DataPubManifest manifest;
    US_DataPubConflictPolicy policy;
    US_DataPublication::ImportTarget target;
    QString outputDir;
    
    // ID remapping: old GUID -> new GUID/ID
    QMap<QString, QString> guidMap;
    QMap<QString, int>     idMap;

    // Import helpers
    bool importProject(const US_DataPubManifestEntry& entry);
    bool importExperiment(const US_DataPubManifestEntry& entry);
    bool importRawData(const US_DataPubManifestEntry& entry);
    bool importBuffer(const US_DataPubManifestEntry& entry);
    bool importAnalyte(const US_DataPubManifestEntry& entry);
    bool importSolution(const US_DataPubManifestEntry& entry);
    bool importModel(const US_DataPubManifestEntry& entry);
    bool importNoise(const US_DataPubManifestEntry& entry);
    bool importRotorCalibration(const US_DataPubManifestEntry& entry);
    
    US_DataPubConflictResult checkConflict(const US_DataPubManifestEntry& entry);
    QString generateUniqueName(const QString& baseName, US_DataPubEntityType type);
    bool verifyDependencies(const US_DataPubManifestEntry& entry);
};

#endif // US_DATA_PUBLICATION_H
