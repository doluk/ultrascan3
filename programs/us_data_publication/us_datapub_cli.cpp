//! \file us_datapub_cli.cpp
#include "us_datapub_cli.h"
#include "us_datapub_catalog.h"

#include "us_settings.h"
#include "us_defines.h"

#include <iostream>

US_DataPubCLI::Args::Args()
{
   source         = "disk";
   target         = "db";
   scope          = "noise";
   conflict       = "reuse";
   timeState      = true;
   dryRun         = false;
   verbose        = false;
   nonInteractive = false;
   verify         = true;
   listOnly       = false;
   help           = false;
   version        = false;
}

US_DataPubCLI::US_DataPubCLI( QObject* parent )
   : QObject( parent ), out( stdout ), err( stderr )
{
   verbose = false;
}

bool US_DataPubCLI::wanted( const QStringList& arguments )
{
   for ( int ii = 1; ii < arguments.size(); ii++ )
   {
      QString arg = arguments[ ii ];

      if ( arg == "--mode"  ||  arg.startsWith( "--mode=" )  ||
           arg == "--no-ui" ||  arg == "--help" || arg == "-h" ||
           arg == "--version" )
         return true;
   }

   return false;
}

QString US_DataPubCLI::usage( void )
{
   QString scopes = US_DataPub::scopeKeys().join( ", " );

   return QObject::tr(
      "us_data_publication -- export and import UltraScan3 data publication"
      " bundles\n"
      "\n"
      "Starting the program without arguments opens the window.\n"
      "\n"
      "Usage:\n"
      "  us_data_publication --mode export --bundle <file.tar.gz> [options]\n"
      "  us_data_publication --mode import --bundle <file.tar.gz> [options]\n"
      "\n"
      "Common options:\n"
      "  --no-ui                 run without the window (implied by --mode)\n"
      "  --db-password <pw>      master password for database access;\n"
      "                          the US3_DB_PASSWORD environment variable is\n"
      "                          used when this is not given\n"
      "  --dry-run               decide everything, write nothing\n"
      "  --non-interactive       never prompt; fail instead of asking\n"
      "  --verbose               report every step\n"
      "  -h, --help              show this text\n"
      "  --version               show the program version\n"
      "\n"
      "Export options:\n"
      "  --source db|disk        where the records are read from"
      " (default: disk)\n"
      "  --project-id <id>       export this project (database ID)\n"
      "  --project-guid <guid>   export this project (GUID)\n"
      "  --experiment-id <run>   export this run; may be repeated.  Accepts a\n"
      "                          runID or an experiment database ID\n"
      "  --max-scope <scope>     export up to and including this record type\n"
      "  --include-<scope>       raise the scope to this record type\n"
      "  --model-guid <guid>     export only these models; may be repeated\n"
      "  --noise-guid <guid>     export only these noise records; repeatable\n"
      "  --no-timestate          leave the runs' time state out\n"
      "  --comment <text>        store a comment in the manifest\n"
      "\n"
      "Import options:\n"
      "  --target db|disk        where the records are written"
      " (default: db)\n"
      "  --output-dir <dir>      disk target root; the UltraScan3 data and\n"
      "                          results directories are used without it\n"
      "  --on-conflict <policy>  reuse, rename or fail (default: reuse)\n"
      "  --on-conflict-<type> <policy>   the policy for one record type\n"
      "  --rename-suffix <text>  suffix for auto-renamed records\n"
      "  --no-verify             skip the payload digest check\n"
      "  --list                  print the manifest and stop\n"
      "\n"
      "Scopes and record types: %1\n"
      "\n"
      "A bundle always holds a prefix of the dependency chain, so that every\n"
      "record in it can be re-created from records that come earlier in the\n"
      "same bundle.  A scope that reaches the raw data is raised to the\n"
      "solutions, because raw data cannot be imported without them.\n" )
      .arg( scopes );
}

void US_DataPubCLI::print( const QString& text )
{
   out << text << "\n";
   out.flush();
}

void US_DataPubCLI::fail( const QString& text )
{
   err << QObject::tr( "Error: " ) << text << "\n";
   err.flush();
}

void US_DataPubCLI::onMessage( const QString& text )
{
   if ( verbose )  print( "  " + text );
}

bool US_DataPubCLI::parse( const QStringList& arguments, QString& error )
{
   QStringList list = arguments.mid( 1 );

   // Split "--option=value" into two entries so that both spellings work
   QStringList flat;

   for ( int ii = 0; ii < list.size(); ii++ )
   {
      QString arg = list[ ii ];

      if ( arg.startsWith( "--" )  &&  arg.contains( "=" ) )
      {
         flat << arg.section( "=", 0, 0 ) << arg.section( "=", 1 );
         continue;
      }

      flat << arg;
   }

   for ( int ii = 0; ii < flat.size(); ii++ )
   {
      QString arg  = flat[ ii ];
      bool    last = ( ii + 1 >= flat.size() );
      QString next = last ? QString() : flat[ ii + 1 ];

      if ( arg == "-h"  ||  arg == "--help" )     { args.help    = true; continue; }
      if ( arg == "--version" )                   { args.version = true; continue; }
      if ( arg == "--no-ui" )                                            continue;
      if ( arg == "--verbose" )        { args.verbose        = true;     continue; }
      if ( arg == "--dry-run" )        { args.dryRun         = true;     continue; }
      if ( arg == "--non-interactive" ){ args.nonInteractive = true;     continue; }
      if ( arg == "--no-verify" )      { args.verify         = false;    continue; }
      if ( arg == "--no-timestate" )   { args.timeState      = false;    continue; }
      if ( arg == "--list" )           { args.listOnly       = true;     continue; }

      if ( arg.startsWith( "--include-" ) )
      {
         QString key   = arg.mid( 10 );
         US_DataPub::Scope scope = US_DataPub::scopeOfKey( key );

         if ( scope == US_DataPub::ScopeNone )
         {
            error = tr( "\"%1\" is not a record type" ).arg( key );
            return false;
         }

         if ( US_DataPub::scopeOfKey( args.scope ) < scope  ||
              args.scope.isEmpty() )
            args.scope = key;

         continue;
      }

      if ( arg.startsWith( "--on-conflict-" ) )
      {
         QString key = arg.mid( 14 );

         if ( last )
         {
            error = tr( "%1 needs a policy" ).arg( arg );
            return false;
         }

         if ( US_DataPub::typeOfKey( key ) == US_DataPub::UnknownType )
         {
            error = tr( "\"%1\" is not a record type" ).arg( key );
            return false;
         }

         args.typeConflicts.insert( key, next );
         ii++;
         continue;
      }

      if ( ! arg.startsWith( "-" ) )
      {
         error = tr( "Unexpected argument \"%1\"" ).arg( arg );
         return false;
      }

      if ( last )
      {
         error = tr( "%1 needs a value" ).arg( arg );
         return false;
      }

      if      ( arg == "--mode"          )  args.mode        = next;
      else if ( arg == "--bundle"        )  args.bundle      = next;
      else if ( arg == "--source"        )  args.source      = next;
      else if ( arg == "--target"        )  args.target      = next;
      else if ( arg == "--output-dir"    )  args.outputDir   = next;
      else if ( arg == "--project-id"    )  args.projectID   = next;
      else if ( arg == "--project-guid"  )  args.projectGUID = next;
      else if ( arg == "--experiment-id" )  args.experiments << next;
      else if ( arg == "--model-guid"    )  args.modelGUIDs  << next;
      else if ( arg == "--noise-guid"    )  args.noiseGUIDs  << next;
      else if ( arg == "--max-scope"     )  args.scope       = next;
      else if ( arg == "--on-conflict"   )  args.conflict    = next;
      else if ( arg == "--rename-suffix" )  args.renameSuffix = next;
      else if ( arg == "--comment"       )  args.comment     = next;
      else if ( arg == "--db-password"   )  args.dbPassword  = next;
      else
      {
         error = tr( "Unknown option \"%1\"" ).arg( arg );
         return false;
      }

      ii++;
   }

   error.clear();

   return true;
}

QString US_DataPubCLI::password( bool needed )
{
   if ( ! needed )  return QString();

   if ( ! args.dbPassword.isEmpty() )  return args.dbPassword;

   QByteArray fromEnv = qgetenv( "US3_DB_PASSWORD" );

   if ( ! fromEnv.isEmpty() )  return QString::fromUtf8( fromEnv );

   if ( args.nonInteractive )  return QString();

   out << tr( "UltraScan master password: " );
   out.flush();

   std::string line;
   std::getline( std::cin, line );

   return QString::fromStdString( line ).trimmed();
}

int US_DataPubCLI::run( const QStringList& arguments )
{
   QString error;

   if ( ! parse( arguments, error ) )
   {
      fail( error );
      print( usage() );
      return 2;
   }

   verbose = args.verbose;

   if ( args.help )
   {
      print( usage() );
      return 0;
   }

   if ( args.version )
   {
      print( QString( "us_data_publication " ) + US_Version );
      return 0;
   }

   if ( args.mode == "export" )  return runExport();
   if ( args.mode == "import" )  return runImport();

   fail( tr( "--mode must be \"export\" or \"import\"" ) );
   print( usage() );

   return 2;
}

int US_DataPubCLI::runExport( void )
{
   if ( args.bundle.isEmpty() )
   {
      fail( tr( "--bundle is required for an export" ) );
      return 2;
   }

   US_DataPub::Scope scope = US_DataPub::scopeOfKey( args.scope );

   if ( scope == US_DataPub::ScopeNone )
   {
      fail( tr( "\"%1\" is not a scope; one of: %2" ).arg( args.scope )
            .arg( US_DataPub::scopeKeys().join( ", " ) ) );
      return 2;
   }

   if ( args.source != "db"  &&  args.source != "disk" )
   {
      fail( tr( "--source must be \"db\" or \"disk\"" ) );
      return 2;
   }

   US_DataPubExporter::Selection selection;
   selection.fromDb           = ( args.source == "db" );
   selection.dbPassword       = password( selection.fromDb );
   selection.scope            = scope;
   selection.projectGUID      = args.projectGUID;
   selection.includeTimeState = args.timeState;
   selection.comment          = args.comment;

   if ( ! args.modelGUIDs.isEmpty() )  selection.setModels( args.modelGUIDs );
   if ( ! args.noiseGUIDs.isEmpty() )  selection.setNoises( args.noiseGUIDs );

   // Resolve the project and the runs against the source
   US_DataPubCatalog catalog;
   QString           error;

   if ( ! catalog.open( selection.fromDb, selection.dbPassword, error ) )
   {
      fail( error );
      return 1;
   }

   if ( ! args.projectID.isEmpty()  &&  selection.projectGUID.isEmpty() )
   {
      QList< US_DataPubCatalog::Project > projects = catalog.projects( error );

      for ( int ii = 0; ii < projects.size(); ii++ )
      {
         if ( projects[ ii ].id != args.projectID )  continue;

         selection.projectGUID = projects[ ii ].guid;
         break;
      }

      if ( selection.projectGUID.isEmpty() )
      {
         fail( tr( "No project with ID %1 was found" ).arg( args.projectID ) );
         return 1;
      }
   }

   if ( ! args.experiments.isEmpty() )
   {
      QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );

      for ( int ii = 0; ii < args.experiments.size(); ii++ )
      {
         QString wanted = args.experiments[ ii ];
         QString runID;

         for ( int jj = 0; jj < runs.size(); jj++ )
         {
            if ( runs[ jj ].runID != wanted  &&  runs[ jj ].id != wanted )
               continue;

            runID = runs[ jj ].runID;
            break;
         }

         if ( runID.isEmpty() )
         {
            fail( tr( "No run \"%1\" was found" ).arg( wanted ) );
            return 1;
         }

         selection.runIDs << runID;
      }
   }

   US_DataPubExporter exporter;
   connect( &exporter, SIGNAL( message( const QString& ) ),
            this,      SLOT  ( onMessage( const QString& ) ) );

   if ( args.dryRun )
   {
      if ( ! exporter.previewManifest( selection, error ) )
      {
         fail( error );
         return 1;
      }

      print( tr( "Dry run: %1 records would be exported to %2" )
             .arg( exporter.manifest().total() ).arg( args.bundle ) );
      print( exporter.manifest().summary() );

      if ( verbose )  print( exporter.manifest().toYaml() );

      return 0;
   }

   if ( ! exporter.exportBundle( selection, args.bundle, error ) )
   {
      fail( error );
      return 1;
   }

   print( tr( "Wrote %1 records to %2" )
          .arg( exporter.manifest().total() ).arg( args.bundle ) );
   print( exporter.manifest().summary() );

   return 0;
}

int US_DataPubCLI::runImport( void )
{
   if ( args.bundle.isEmpty() )
   {
      fail( tr( "--bundle is required for an import" ) );
      return 2;
   }

   US_DataPubImporter importer;
   connect( &importer, SIGNAL( message( const QString& ) ),
            this,      SLOT  ( onMessage( const QString& ) ) );

   QString error;

   if ( ! importer.inspect( args.bundle, error ) )
   {
      fail( error );
      return 1;
   }

   if ( args.listOnly )
   {
      print( importer.manifest().toYaml() );
      return 0;
   }

   if ( args.target != "db"  &&  args.target != "disk" )
   {
      fail( tr( "--target must be \"db\" or \"disk\"" ) );
      return 2;
   }

   bool ok = true;
   US_DataPubImporter::Options options;
   options.target       = ( args.target == "db" ) ? US_DataPub::TargetDb
                                                  : US_DataPub::TargetDisk;
   options.dbPassword   = password( options.target == US_DataPub::TargetDb );
   options.outputDir    = args.outputDir;
   options.dryRun       = args.dryRun;
   options.verifyHashes = args.verify;
   options.policy       = US_DataPub::policyOfKey( args.conflict, &ok );

   if ( ! ok )
   {
      fail( tr( "--on-conflict must be \"reuse\", \"rename\" or \"fail\"" ) );
      return 2;
   }

   if ( ! args.renameSuffix.isEmpty() )
      options.renameSuffix = args.renameSuffix;

   QList< QString > keys = args.typeConflicts.keys();

   for ( int ii = 0; ii < keys.size(); ii++ )
   {
      US_DataPub::EntityType type = US_DataPub::typeOfKey( keys[ ii ] );
      US_DataPub::ConflictPolicy policy =
            US_DataPub::policyOfKey( args.typeConflicts.value( keys[ ii ] ),
                                     &ok );

      if ( ! ok )
      {
         fail( tr( "\"%1\" is not a conflict policy" )
               .arg( args.typeConflicts.value( keys[ ii ] ) ) );
         return 2;
      }

      options.typePolicies.insert( int( type ), policy );
   }

   if ( ! importer.runImport( options, error ) )
   {
      fail( error );
      return 1;
   }

   QList< US_DataPubImporter::Result > results = importer.results();
   int created = 0;
   int reused  = 0;
   int renamed = 0;

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      switch ( results[ ii ].resolution )
      {
         case US_DataPub::ResolvedCreated:  created++;  break;
         case US_DataPub::ResolvedReused:   reused++;   break;
         case US_DataPub::ResolvedRenamed:  renamed++;  break;
         default:                                       break;
      }

      if ( ! verbose )  continue;

      print( QString( "  %1  %2 -> %3 (%4)" )
             .arg( US_DataPub::typeText( results[ ii ].type ) )
             .arg( results[ ii ].name )
             .arg( results[ ii ].targetName )
             .arg( US_DataPub::resolutionText( results[ ii ].resolution ) ) );
   }

   print( args.dryRun
          ? tr( "Dry run: %1 records would be created, %2 reused, %3 renamed" )
            .arg( created ).arg( reused ).arg( renamed )
          : tr( "Imported %1 records: %2 created, %3 reused, %4 renamed" )
            .arg( results.size() ).arg( created ).arg( reused )
            .arg( renamed ) );

   return 0;
}
