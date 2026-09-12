//! \file us_data_publication.h
#ifndef US_DATA_PUBLICATION_H
#define US_DATA_PUBLICATION_H

#include "us_widgets.h"
#include "us_help.h"

#include "us_datapub_export_pane.h"
#include "us_datapub_import_pane.h"

/*! \class US_DataPublication
    \brief The window of the data publication tool

    Exporting and importing are two different jobs, so they get a tab each
    and never share a control.  Each tab starts with its own disk/database
    selector, which is what decides where an export reads its records from
    and where an import writes them to.
*/
class US_DataPublication : public US_Widgets
{
   Q_OBJECT

   public:
      //! \brief Build the window
      US_DataPublication();

   private:
      QTabWidget*             tabs;
      US_DataPubExportPane*   exportPane;
      US_DataPubImportPane*   importPane;
      QLabel*                 lb_status;

      US_Help                 showHelp;

   private slots:
      void set_status( const QString& );
      void help      ( void )
      { showHelp.show_help( "us_data_publication.html" ); };
};
#endif
