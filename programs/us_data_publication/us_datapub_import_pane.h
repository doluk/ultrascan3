//! \file us_datapub_import_pane.h
#ifndef US_DATAPUB_IMPORT_PANE_H
#define US_DATAPUB_IMPORT_PANE_H

#include "us_widgets.h"
#include "us_widgets_dialog.h"
#include "us_help.h"

#include "us_datapub_defs.h"
#include "us_datapub_import.h"

/*! \class US_DataPubConflictDialog
    \brief Asks the user what to do about one name conflict

    The dialog shows what the record in the bundle and the record already in
    the target have in common, and offers the only two safe ways out:
    pointing at the existing record, or importing under a new name.  The
    reuse choice is disabled when the two records differ, because reusing a
    record that differs would silently change what the imported data means.
*/
class US_DataPubConflictDialog : public US_WidgetsDialog
{
   Q_OBJECT

   public:
      //! \brief Build the conflict dialog
      //! \param type      The record type of the conflict
      //! \param name      The name that is already taken in the target
      //! \param identical True when the two records have the same properties
      //! \param details   A description of how the records differ
      //! \param newName   The suggested new name
      //! \param parent    The parent widget, for Qt ownership
      US_DataPubConflictDialog( US_DataPub::EntityType type,
                                const QString& name, bool identical,
                                const QString& details,
                                const QString& newName,
                                QWidget* parent = nullptr );

      //! \brief The policy the user picked
      US_DataPub::ConflictPolicy policy( void ) const;

      //! \brief The name the user gave the record
      QString chosenName( void ) const;

      //! \brief True when the user asked to apply the answer to every
      //!        further conflict of this record type
      bool    applyToAll( void ) const;

   private:
      US_DataPub::ConflictPolicy result;
      QLineEdit*  le_name;
      QCheckBox*  ck_all;

   private slots:
      void chose_reuse ( void );
      void chose_rename( void );
      void chose_cancel( void );
};

/*! \class US_DataPubImportPane
    \brief The import tab of the data publication window

    The tab inspects a bundle, shows what is in it, and replays it into the
    database or into a local disk store.  Nothing in the target is ever
    overwritten: a record that is already there is either pointed at or
    imported again under a new name.
*/
class US_DataPubImportPane : public US_Widgets,
                             public US_DataPubConflictResolver
{
   Q_OBJECT

   public:
      //! \brief Build the import tab
      //! \param parent The parent widget, for Qt ownership
      explicit US_DataPubImportPane( QWidget* parent = nullptr );

      //! \brief Answer one conflict, by dialog or by the chosen policy
      //! \param type      The record type of the conflict
      //! \param name      The name that is already taken in the target
      //! \param identical True when the two records have the same properties
      //! \param details   A description of how the records differ
      //! \param newName   The suggested new name; may be changed
      US_DataPub::ConflictPolicy resolve( US_DataPub::EntityType type,
                                          const QString& name, bool identical,
                                          const QString& details,
                                          QString& newName ) override;

   signals:
      //! \brief A note for the window's status line
      //! \param message The note
      void status( const QString& message );

   private:
      US_Disk_DB_Controls* dkdb_cntrls;

      QPushButton*  pb_browse;
      QPushButton*  pb_inspect;
      QPushButton*  pb_outdir;
      QPushButton*  pb_details;
      QPushButton*  pb_reset;
      QPushButton*  pb_import;

      QLineEdit*    le_bundle;
      QLineEdit*    le_outdir;
      QLineEdit*    le_summary;

      QComboBox*    cb_policy;
      QCheckBox*    ck_dryrun;
      QCheckBox*    ck_verify;
      QCheckBox*    ck_prompt;

      QTreeWidget*  tw_bundle;
      QProgressBar* pgb_progress;
      QTextEdit*    te_status;

      US_Help       showHelp;

      US_DataPubImporter importer;
      bool               inspected;

      QMap< int, US_DataPub::ConflictPolicy > sticky;

      void buildTree    ( void );
      void updateSummary( void );

   private slots:
      void target_changed( bool );
      void browse_bundle ( void );
      void browse_outdir ( void );
      void inspect       ( void );
      void show_details  ( void );
      void run_import    ( void );
      void reset         ( void );
      void importer_note ( const QString& );
      void importer_steps( int );
      void importer_step ( int );
      void help          ( void )
      { showHelp.show_help( "us_data_publication.html" ); };
};
#endif
