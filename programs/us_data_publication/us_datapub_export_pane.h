//! \file us_datapub_export_pane.h
#ifndef US_DATAPUB_EXPORT_PANE_H
#define US_DATAPUB_EXPORT_PANE_H

#include "us_widgets.h"
#include "us_help.h"
#include "us_project.h"

#include "us_datapub_defs.h"
#include "us_datapub_catalog.h"
#include "us_datapub_export.h"

/*! \class US_DataPubExportPane
    \brief The export tab of the data publication window

    The tab walks the user down the dependency chain: a project narrows the
    runs, the runs bring their raw data and edits into a tree of check
    boxes, and the models picked from the model loader bring their edits --
    and their noise -- along with them.  The summary at the bottom always
    says what the bundle would contain.
*/
class US_DataPubExportPane : public US_Widgets
{
   Q_OBJECT

   public:
      //! \brief Build the export tab
      //! \param parent The parent widget, for Qt ownership
      explicit US_DataPubExportPane( QWidget* parent = nullptr );

   signals:
      //! \brief A note for the window's status line
      //! \param message The note
      void status( const QString& message );

   private:
      US_Disk_DB_Controls* dkdb_cntrls;

      QPushButton*  pb_project;
      QPushButton*  pb_clearproj;
      QPushButton*  pb_runs;
      QPushButton*  pb_clearruns;
      QPushButton*  pb_allraw;
      QPushButton*  pb_noraw;
      QPushButton*  pb_lastedit;
      QPushButton*  pb_alledit;
      QPushButton*  pb_noedit;
      QPushButton*  pb_models;
      QPushButton*  pb_noises;
      QPushButton*  pb_clearmodels;
      QPushButton*  pb_browse;
      QPushButton*  pb_details;
      QPushButton*  pb_reset;
      QPushButton*  pb_export;

      QLineEdit*    le_project;
      QLineEdit*    le_runs;
      QLineEdit*    le_bundle;
      QLineEdit*    le_summary;
      QLineEdit*    le_comment;

      QComboBox*    cb_scope;
      QCheckBox*    ck_tmst;

      QTreeWidget*  tw_data;
      QTreeWidget*  tw_models;

      QProgressBar* pgb_progress;
      QTextEdit*    te_status;

      US_Help       showHelp;

      US_DataPubCatalog catalog;
      bool              catalog_open;
      bool              updating;

      QString           project_guid;
      QString           project_id;
      QString           project_desc;

      QList< US_DataPubCatalog::Run >   runs;
      QList< US_DataPubCatalog::Model > models;
      QList< US_DataPubCatalog::Noise > noises;
      QStringList                       sel_models;
      QStringList                       sel_noises;

      bool    openCatalog( void );
      QString password   ( void );

      void    buildDataTree ( void );
      void    buildModelTree( void );
      void    updateSummary ( void );

      QStringList checkedRaws ( void ) const;
      QStringList checkedEdits( void ) const;

      QTreeWidgetItem* itemFor( QTreeWidget*, const QString& guid ) const;

      void    setEditChecks( const QString& mode );
      bool    modelsNeeding( const QString& editGUID, QStringList& ) const;

      US_DataPubExporter::Selection selection( void ) const;

   private slots:
      void source_changed ( bool );
      void select_project ( void );
      void project_chosen ( US_Project& );
      void clear_project  ( void );
      void select_runs    ( void );
      void clear_runs     ( void );
      void all_raw        ( void );
      void no_raw         ( void );
      void latest_edits   ( void );
      void all_edits      ( void );
      void no_edits       ( void );
      void select_models  ( void );
      void select_noises  ( void );
      void clear_models   ( void );
      void item_changed   ( QTreeWidgetItem*, int );
      void browse_bundle  ( void );
      void show_details   ( void );
      void reset          ( void );
      void run_export     ( void );
      void exporter_note  ( const QString& );
      void exporter_steps ( int );
      void exporter_step  ( int );
      void help           ( void )
      { showHelp.show_help( "us_data_publication.html" ); };
};
#endif
