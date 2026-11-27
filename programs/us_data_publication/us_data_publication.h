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

   protected:
      /*! \brief Wait for a running export or import before closing

          The window does not close while either tab is at work; it
          remembers that it was asked to and closes once the work is done.
      */
      void closeEvent( QCloseEvent* event ) override;

   private:
      QTabWidget*             tabs;
      US_DataPubExportPane*   exportPane;
      US_DataPubImportPane*   importPane;
      QLabel*                 lb_status;

      bool                    close_pending;   //!< Close when work ends
      bool                    import_signals;  //!< Import tab has busyChanged

      US_Help                 showHelp;

   private slots:
      void set_status( const QString& );

      //! \brief Lock the other tab while one is at work
      void pane_busy ( bool );
      void help      ( void )
      { showHelp.show_help( "us_data_publication.html" ); };
};
#endif
