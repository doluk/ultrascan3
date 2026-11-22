//! \file us_data_publication.cpp
#include "us_data_publication.h"
#include "us_datapub_cli.h"

#include "us_gui_settings.h"
#include "us_settings.h"
#include "us_license_t.h"
#include "us_license.h"
#include "us_defines.h"

//! \brief Main program for US_DataPublication
//!
//! Without arguments the window opens; with any long option (\c --mode,
//! \c --no-ui, \c --help, ...) the command line runs instead, on a console
//! application object so that the program works on a machine with no
//! display, and reports arguments it does not understand.
//!
//! Which of the two it is only needs the ASCII "--" in front of an
//! argument, so it is decided from the raw arguments; the arguments the
//! command line works with are the application's, which Qt decodes
//! correctly on every platform (the command line of a Windows process is
//! UTF-16, which the local 8-bit code page cannot hold).
int main( int argc, char* argv[] )
{
   QStringList raw;

   for ( int ii = 0; ii < argc; ii++ )
      raw << QString::fromLatin1( argv[ ii ] );

   if ( US_DataPubCLI::wanted( raw ) )
   {
      QCoreApplication console( argc, argv );
      QCoreApplication::setOrganizationName( "UltraScan" );
      QCoreApplication::setApplicationName ( "us_data_publication" );

      US_DataPubCLI cli;

      return cli.run( QCoreApplication::arguments() );
   }

   QApplication application( argc, argv );

   #include "main1.inc"

   // License is OK.  Start up.

   US_DataPublication w;
   w.show();                   //!< \memberof QWidget
   return application.exec();  //!< \memberof QApplication
}

US_DataPublication::US_DataPublication() : US_Widgets()
{
   setWindowTitle( tr( "Data Publication: Export and Import Bundles" ) );
   setPalette( US_GuiSettings::frameColor() );

   QVBoxLayout* main = new QVBoxLayout( this );
   main->setContentsMargins( 2, 2, 2, 2 );
   main->setSpacing        ( 2 );

   tabs       = new QTabWidget( this );
   exportPane = new US_DataPubExportPane( this );
   importPane = new US_DataPubImportPane( this );

   tabs->addTab( exportPane, tr( "Export" ) );
   tabs->addTab( importPane, tr( "Import" ) );
   main->addWidget( tabs );

   lb_status  = us_label( tr( "Ready." ) );
   main->addWidget( lb_status );

   connect( exportPane, &US_DataPubExportPane::status,
            this,       &US_DataPublication::set_status );
   connect( importPane, &US_DataPubImportPane::status,
            this,       &US_DataPublication::set_status );

   // While one tab works, events are processed to keep the window alive;
   // nothing in the other tab -- and no switch to it -- may start then
   connect( exportPane, &US_DataPubExportPane::busyChanged,
            this,       &US_DataPublication::pane_busy );

   // The import tab announces the same; looked up by name, so that a build
   // whose import tab does not have the signal still closes cleanly (it
   // then only refuses to close while an import runs)
   import_signals = ( importPane->metaObject()->indexOfSignal(
                      "busyChanged(bool)" ) >= 0 );

   if ( import_signals )
      connect( importPane, SIGNAL( busyChanged( bool ) ),
               this,       SLOT  ( pane_busy  ( bool ) ) );

   close_pending = false;

   resize( 1100, 800 );
}

void US_DataPublication::set_status( const QString& message )
{
   lb_status->setText( message );
}

void US_DataPublication::pane_busy( bool )
{
   bool exporting = exportPane->isBusy();
   bool importing = importPane->isBusy();
   bool working   = exporting  ||  importing;

   tabs->tabBar()->setEnabled( ! working );
   exportPane->setEnabled( ! importing );
   importPane->setEnabled( ! exporting );

   // A close that was asked for while the work ran happens now
   if ( ! working  &&  close_pending )
   {
      close_pending = false;
      QTimer::singleShot( 0, this, &QWidget::close );
   }
}

/* The export runs on this window's thread and keeps it responsive by
   processing events; closing the window then would destroy the objects the
   export is still working with.
*/
void US_DataPublication::closeEvent( QCloseEvent* event )
{
   if ( exportPane->isBusy() )
   {
      close_pending = true;
      lb_status->setText( tr( "An export is running; the window closes"
                              " once it is done." ) );
      event->ignore();
      return;
   }

   if ( importPane->isBusy() )
   {
      // Without the signal the end of the import is not seen here
      close_pending = import_signals;
      lb_status->setText( import_signals
         ? tr( "An import is running; the window closes once it is done." )
         : tr( "An import is running; close the window once it is"
               " done." ) );
      event->ignore();
      return;
   }

   US_Widgets::closeEvent( event );
}
