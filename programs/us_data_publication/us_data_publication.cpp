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
//! Without arguments the window opens; with \c --mode, \c --no-ui or
//! \c --help the command line runs instead, on a console application object
//! so that the program works on a machine with no display.
int main( int argc, char* argv[] )
{
   QStringList arguments;

   for ( int ii = 0; ii < argc; ii++ )
      arguments << QString::fromLocal8Bit( argv[ ii ] );

   if ( US_DataPubCLI::wanted( arguments ) )
   {
      QCoreApplication console( argc, argv );
      QCoreApplication::setOrganizationName( "UltraScan" );
      QCoreApplication::setApplicationName ( "us_data_publication" );

      US_DataPubCLI cli;

      return cli.run( arguments );
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

   resize( 1100, 800 );
}

void US_DataPublication::set_status( const QString& message )
{
   lb_status->setText( message );
}
