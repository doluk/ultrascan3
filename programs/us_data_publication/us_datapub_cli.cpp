//! \file us_datapub_cli.cpp
#include "us_datapub_cli.h"
#include "us_datapub_catalog.h"

#include "us_settings.h"
#include "us_defines.h"

#include <cerrno>
#include <cstring>
#include <iostream>

#if defined( Q_OS_UNIX )
#include <csignal>
#include <termios.h>
#include <unistd.h>
#elif defined( Q_OS_WIN )
#include <windows.h>
#endif

namespace
{
#if defined( Q_OS_UNIX )
   // The terminal settings to go back to, and the quiet ones the prompt
   // reads with, also from a signal handler
   termios               saved_tty;
   termios               quiet_tty;
   volatile sig_atomic_t tty_quiet = 0;

   // SIGTSTP too: Ctrl-Z at the prompt stops the process, and the shell
   // the user returns to must echo
   const int             guarded[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT,
                                       SIGTSTP };
   const int             nguarded  = int( sizeof( guarded ) / sizeof( int ) );
   struct sigaction      old_actions[ sizeof( guarded ) / sizeof( int ) ];

   /* A signal at the prompt must not leave the terminal without echo: put
      it back, restore what the signal did before and deliver it again.

      When that returns -- the signal was ignored or handled elsewhere, or
      (SIGTSTP) the process was stopped and has been continued -- the
      prompt is still reading, so this handler goes back in place and the
      terminal goes quiet again; the guard is installed with SA_RESTART, so
      the read itself carries on rather than failing.
   */
   void restore_tty_and_reraise( int sig )
   {
      int  saved_errno = errno;
      bool was_quiet   = ( tty_quiet != 0 );
      int  index       = -1;

      if ( was_quiet )
         tcsetattr( STDIN_FILENO, TCSANOW, &saved_tty );

      tty_quiet = 0;

      for ( int ii = 0; ii < nguarded; ii++ )
         if ( guarded[ ii ] == sig )  index = ii;

      struct sigaction mine;
      memset( &mine, 0, sizeof( mine ) );

      if ( index >= 0 )
         sigaction( sig, &old_actions[ index ], &mine );

      // The signal is blocked while its handler runs; let it through now
      sigset_t unblock;
      sigemptyset( &unblock );
      sigaddset  ( &unblock, sig );
      sigprocmask( SIG_UNBLOCK, &unblock, nullptr );

      raise( sig );

      // Still reading: back to the guard, and to no echo
      if ( index >= 0 )
         sigaction( sig, &mine, nullptr );

      if ( was_quiet )
      {
         tcsetattr( STDIN_FILENO, TCSANOW, &quiet_tty );
         tty_quiet = 1;
      }

      errno = saved_errno;
   }
#elif defined( Q_OS_WIN )
   HANDLE         console_in   = INVALID_HANDLE_VALUE;
   DWORD          console_mode = 0;
   volatile LONG  console_quiet = 0;

   BOOL WINAPI restore_console( DWORD )
   {
      if ( console_quiet )
         SetConsoleMode( console_in, console_mode );

      console_quiet = 0;

      return FALSE;                   // Let the default handler go on
   }
#endif

   /* Read one line from the terminal without showing what is typed.

      Returns false, with a reason, when there is no way to read it
      without echoing it.  The line comes back without its line end.
   */
   bool read_hidden_line( QString& line, QString& error )
   {
      line.clear();
      error.clear();

#if defined( Q_OS_UNIX )
      bool tty = ( isatty( STDIN_FILENO )  &&
                   tcgetattr( STDIN_FILENO, &saved_tty ) == 0 );

      if ( tty )
      {
         struct sigaction guard;
         memset( &guard, 0, sizeof( guard ) );
         guard.sa_handler = restore_tty_and_reraise;
         guard.sa_flags   = SA_RESTART;
         sigemptyset( &guard.sa_mask );

         quiet_tty = saved_tty;
         quiet_tty.c_lflag &= ~ECHO;

         for ( int ii = 0; ii < nguarded; ii++ )
            sigaction( guarded[ ii ], &guard, &old_actions[ ii ] );

         tty_quiet = 1;
         tcsetattr( STDIN_FILENO, TCSANOW, &quiet_tty );
      }

      std::string bytes;
      std::getline( std::cin, bytes );

      if ( tty )
      {
         tcsetattr( STDIN_FILENO, TCSANOW, &saved_tty );
         tty_quiet = 0;

         for ( int ii = 0; ii < nguarded; ii++ )
            sigaction( guarded[ ii ], &old_actions[ ii ], nullptr );

         std::cerr << std::endl;
      }

      // UTF-8, as a password file and US3_DB_PASSWORD are: one password
      // has to give the same bytes whichever way it is given
      line = QString::fromUtf8( bytes.data(), int( bytes.size() ) );
#elif defined( Q_OS_WIN )
      console_in = GetStdHandle( STD_INPUT_HANDLE );

      if ( ! GetConsoleMode( console_in, &console_mode ) )
      {
         error = QObject::tr( "the input is not a console, so the password"
                              " cannot be asked for without showing it" );
         return false;
      }

      SetConsoleCtrlHandler( restore_console, TRUE );
      console_quiet = 1;
      SetConsoleMode( console_in, ( console_mode & ~ENABLE_ECHO_INPUT )
                                  | ENABLE_LINE_INPUT );

      // Read as UTF-16, whatever the console's code page
      std::wstring text;
      wchar_t      buffer[ 256 ];
      DWORD        nread = 0;

      while ( ReadConsoleW( console_in, buffer, 256, &nread, nullptr )  &&
              nread > 0 )
      {
         text.append( buffer, nread );

         if ( text.find( L'\n' ) != std::wstring::npos )  break;
      }

      SetConsoleMode( console_in, console_mode );
      console_quiet = 0;
      SetConsoleCtrlHandler( restore_console, FALSE );
      std::cerr << std::endl;

      line = QString::fromStdWString( text );
#else
      error = QObject::tr( "this platform cannot read a password without"
                           " showing it" );
      return false;
#endif

      line = US_DataPubCLI::chopLineEnd( line );

      return true;
   }

   // The options that belong to one mode only
   const QStringList& export_options( void )
   {
      static const QStringList keys = QStringList()
         << "--source" << "--project-id" << "--experiment-id"
         << "--model-guid" << "--noise-guid" << "--max-scope"
         << "--include-<scope>" << "--no-timestate" << "--no-project"
         << "--comment";
      return keys;
   }

   const QStringList& import_options( void )
   {
      static const QStringList keys = QStringList()
         << "--target" << "--output-dir" << "--on-conflict"
         << "--on-conflict-<type>" << "--rename-suffix" << "--no-verify"
         << "--strict-hardware" << "--list";
      return keys;
   }
}

US_DataPubCLI::Args::Args()
{
   source         = "disk";
   target         = "db";
   conflict       = "reuse";
   timeState      = true;
   project        = true;
   dryRun         = false;
   verbose        = false;
   nonInteractive = false;
   verify         = true;
   listOnly       = false;
   strictHardware = false;
   help           = false;
   version        = false;
}

US_DataPubCLI::US_DataPubCLI( QObject* parent )
   : QObject( parent ), out( stdout ), err( stderr )
{
   verbose         = false;
   password_failed = false;
}

bool US_DataPubCLI::wanted( const QStringList& arguments )
{
   for ( int ii = 1; ii < arguments.size(); ii++ )
   {
      QString arg = arguments[ ii ];

      if ( arg.startsWith( "--" )  ||  arg == "-h" )
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
      "  --db-password-file <f>  read the master password for database\n"
      "                          access from the first line of a file; only\n"
      "                          the line end is removed, and a file other\n"
      "                          users can read is warned about\n"
      "  --db-password <pw>      the master password itself; other users can\n"
      "                          see it in the process list, so prefer the\n"
      "                          file or the US3_DB_PASSWORD environment\n"
      "                          variable, which is used when neither is\n"
      "                          given.  Without any of them an interactive\n"
      "                          run asks for it\n"
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
      "                          (default: noise, the whole chain)\n"
      "  --include-<scope>       export up to and including this record type;\n"
      "                          the deepest one given counts, and it may not\n"
      "                          go beyond --max-scope\n"
      "  --model-guid <guid>     export only these models; may be repeated\n"
      "  --noise-guid <guid>     export only these noise records; repeatable\n"
      "  --no-timestate          leave the runs' time state out\n"
      "  --no-project            leave the project record out; the runs still\n"
      "                          name it, so an import can attach them to a\n"
      "                          project the target already has\n"
      "  --comment <text>        store a comment in the manifest\n"
      "\n"
      "Import options:\n"
      "  --target db|disk        where the records are written"
      " (default: db)\n"
      "  --output-dir <dir>      disk target root; the UltraScan3 data and\n"
      "                          results directories are used without it\n"
      "  --project-guid <guid>   attach the imported runs to this project of\n"
      "                          the target; it has to exist there already,\n"
      "                          and the bundle\'s own project record is then\n"
      "                          reused rather than created\n"
      "  --on-conflict <policy>  reuse, rename or fail (default: reuse)\n"
      "  --on-conflict-<type> <policy>   the policy for one record type\n"
      "  --rename-suffix <text>  suffix for auto-renamed records\n"
      "  --no-verify             skip the payload digest check; the only\n"
      "                          way to import a bundle without digests\n"
      "  --strict-hardware       fail rather than stand another rotor,\n"
      "                          calibration, centerpiece, instrument or\n"
      "                          operator of the target in for a missing one\n"
      "  --list                  print the manifest and stop\n"
      "\n"
      "An option of the other mode (--target with an export, --model-guid\n"
      "with an import, ...) is refused as a usage error.\n"
      "\n"
      "An import exits with 0 when every record was imported, 1 when it\n"
      "failed (a database import is then rolled back) and 2 on a usage\n"
      "error.  Substitutions and records that could not be imported are\n"
      "reported as warnings on the error output.\n"
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

namespace
{
   // How many errors this process has reported, so that a caller can tell
   // whether a step it called already said why it failed
   int errors_reported = 0;
}

void US_DataPubCLI::fail( const QString& text )
{
   errors_reported++;
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

   // Split "--option=value" into two entries so that both spellings work.
   // A value given that way is taken as it is, even when it starts with
   // "--"; a separate one that looks like an option is not a value.
   QStringList   flat;
   QList< bool > inline_value;

   for ( int ii = 0; ii < list.size(); ii++ )
   {
      QString arg = list[ ii ];

      if ( arg.startsWith( "--" )  &&  arg.contains( "=" ) )
      {
         flat << arg.section( "=", 0, 0 ) << arg.section( "=", 1 );
         inline_value << false << true;
         continue;
      }

      flat << arg;
      inline_value << false;
   }

   for ( int ii = 0; ii < flat.size(); ii++ )
   {
      QString arg  = flat[ ii ];
      bool    last = ( ii + 1 >= flat.size() );
      QString next = last ? QString() : flat[ ii + 1 ];

      if ( ! last  &&  ! inline_value[ ii + 1 ]  &&  next.startsWith( "--" ) )
      {  // "--bundle --dry-run": the option has no value of its own
         last = true;
         next.clear();
      }

      if ( inline_value[ ii ] )
      {
         error = tr( "Unexpected argument \"%1\"" ).arg( arg );
         return false;
      }

      // What was given, so that options of the other mode can be refused
      if ( arg.startsWith( "--" ) )
         args.given << arg;

      if ( arg == "-h"  ||  arg == "--help" )     { args.help    = true; continue; }
      if ( arg == "--version" )                   { args.version = true; continue; }
      if ( arg == "--no-ui" )                                            continue;
      if ( arg == "--verbose" )        { args.verbose        = true;     continue; }
      if ( arg == "--dry-run" )        { args.dryRun         = true;     continue; }
      if ( arg == "--non-interactive" ){ args.nonInteractive = true;     continue; }
      if ( arg == "--no-verify" )      { args.verify         = false;    continue; }
      if ( arg == "--no-timestate" )   { args.timeState      = false;    continue; }
      if ( arg == "--no-project" )     { args.project        = false;    continue; }
      if ( arg == "--list" )           { args.listOnly       = true;     continue; }
      if ( arg == "--strict-hardware" ){ args.strictHardware = true;     continue; }

      if ( arg.startsWith( "--include-" ) )
      {
         QString key   = arg.mid( 10 );
         US_DataPub::Scope scope = US_DataPub::scopeOfKey( key );

         if ( scope == US_DataPub::ScopeNone )
         {
            error = tr( "\"%1\" is not a record type" ).arg( key );
            return false;
         }

         if ( args.includeScope.isEmpty()  ||
              US_DataPub::scopeOfKey( args.includeScope ) < scope )
            args.includeScope = key;

         if ( ! last  &&  inline_value[ ii + 1 ] )
         {
            error = tr( "%1 takes no value" ).arg( arg );
            return false;
         }

         continue;
      }

      if ( arg.startsWith( "--on-conflict-" ) )
      {
         QString key = arg.mid( 14 );

         if ( last  ||  next.isEmpty() )
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

      static const QStringList withValue = QStringList()
         << "--mode" << "--bundle" << "--source" << "--target"
         << "--output-dir" << "--project-id" << "--project-guid"
         << "--experiment-id" << "--model-guid" << "--noise-guid"
         << "--max-scope" << "--on-conflict" << "--rename-suffix"
         << "--comment" << "--db-password" << "--db-password-file";

      if ( ! withValue.contains( arg ) )
      {
         error = tr( "Unknown option \"%1\"" ).arg( arg );
         return false;
      }

      if ( last  ||  ( next.isEmpty()  &&  arg != "--comment" ) )
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
      else if ( arg == "--db-password-file" )  args.dbPasswordFile = next;
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

QString US_DataPubCLI::chopLineEnd( const QString& text )
{
   if ( text.endsWith( "\r\n" ) )  return text.left( text.size() - 2 );

   if ( text.endsWith( "\n" )  ||  text.endsWith( "\r" ) )
      return text.left( text.size() - 1 );

   return text;
}

bool US_DataPubCLI::readPasswordFile( const QString& path, QString& password,
                                      QString& warning, QString& error )
{
   password.clear();
   warning .clear();
   error   .clear();

   QFile file( path );

   if ( ! file.open( QIODevice::ReadOnly ) )
   {
      error = tr( "The password file %1 cannot be read" ).arg( path );
      return false;
   }

   QByteArray line   = file.readLine( 64 * 1024 );
   bool       failed = ( file.error() != QFileDevice::NoError );
   file.close();

   if ( failed )
   {
      error = tr( "The password file %1 cannot be read: %2" ).arg( path )
              .arg( file.errorString() );
      return false;
   }

   password = chopLineEnd( QString::fromUtf8( line ) );

#if defined( Q_OS_UNIX )
   QFileDevice::Permissions perms = QFile::permissions( path );

   if ( perms & ( QFileDevice::ReadGroup | QFileDevice::ReadOther ) )
      warning = tr( "the password file %1 can be read by other users;"
                    " restrict it with \"chmod 600 %1\"" ).arg( path );
#endif

   return true;
}

QString US_DataPubCLI::resolveRun( const QList< US_DataPubCatalog::Run >& runs,
                                   const QString& wanted, QString& why )
{
   why.clear();

   QStringList byID;

   for ( int ii = 0; ii < runs.size(); ii++ )
      if ( runs[ ii ].id == wanted  &&  ! byID.contains( runs[ ii ].runID ) )
         byID << runs[ ii ].runID;

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      if ( runs[ ii ].runID != wanted )  continue;

      byID.removeAll( wanted );

      if ( ! byID.isEmpty() )
         why = tr( "\"%1\" is taken as the run ID %1; it is also the database"
                   " ID of run %2, which is not exported" )
               .arg( wanted ).arg( byID.join( ", " ) );

      return runs[ ii ].runID;
   }

   if ( byID.size() == 1 )  return byID[ 0 ];

   if ( byID.size() > 1 )
      why = tr( "\"%1\" is the ID of %2 runs (%3); name the run by its run"
                " ID" ).arg( wanted ).arg( byID.size() )
            .arg( byID.join( ", " ) );
   else
      why = tr( "No run \"%1\" was found" ).arg( wanted );

   return QString();
}

QString US_DataPubCLI::password( bool needed )
{
   password_failed = false;

   if ( ! needed )  return QString();

   if ( ! args.dbPasswordFile.isEmpty() )
   {
      QString pw;
      QString warning;
      QString error;

      if ( ! readPasswordFile( args.dbPasswordFile, pw, warning, error ) )
      {
         fail( error );
         password_failed = true;
         return QString();
      }

      if ( ! warning.isEmpty() )
      {
         err << tr( "Warning: " ) << warning << "\n";
         err.flush();
      }

      return pw;
   }

   if ( ! args.dbPassword.isEmpty() )
   {
      err << tr( "Warning: a password given with --db-password can be seen"
                 " by other users of this machine; prefer --db-password-file"
                 " or US3_DB_PASSWORD" ) << "\n";
      err.flush();
      return args.dbPassword;
   }

   // qEnvironmentVariable(), not qgetenv(): on Windows the environment is
   // UTF-16, and the 8-bit copy loses what the code page cannot hold
   QString fromEnv = qEnvironmentVariable( "US3_DB_PASSWORD" );

   if ( ! fromEnv.isEmpty() )
      return chopLineEnd( fromEnv );

   if ( args.nonInteractive )
   {
      fail( tr( "The database needs the master password, and a"
                " non-interactive run cannot ask for it: give"
                " --db-password-file, or set US3_DB_PASSWORD" ) );
      return QString();
   }

   // The prompt goes to the error output, so that it is seen even when
   // the regular output is redirected
   err << tr( "UltraScan master password: " );
   err.flush();

   QString line;
   QString error;

   if ( ! read_hidden_line( line, error ) )
   {
      err << "\n";
      fail( tr( "The password cannot be asked for: %1; give"
                " --db-password-file, or set US3_DB_PASSWORD" ).arg( error ) );
      password_failed = true;
      return QString();
   }

   return line;
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

   if ( args.mode == "export"  ||  args.mode == "import" )
   {
      // An option of the other mode would be ignored; say so instead
      const QStringList& other = ( args.mode == "export" ) ? import_options()
                                                          : export_options();
      QStringList        wrong;

      for ( int ii = 0; ii < args.given.size(); ii++ )
      {
         QString given  = args.given[ ii ];
         QString family = given;

         if ( given.startsWith( "--include-" ) )
            family = "--include-<scope>";
         else if ( given.startsWith( "--on-conflict-" ) )
            family = "--on-conflict-<type>";

         if ( other.contains( family )  &&  ! wrong.contains( given ) )
            wrong << given;
      }

      if ( ! wrong.isEmpty() )
      {
         fail( tr( "%1 cannot be used with --mode %2" )
               .arg( wrong.join( ", " ) ).arg( args.mode ) );
         return 2;
      }

      return ( args.mode == "export" ) ? runExport() : runImport();
   }

   fail( tr( "--mode must be \"export\" or \"import\"" ) );
   print( usage() );

   return 2;
}

int US_DataPubCLI::runExport( void )
{
   if ( args.bundle.isEmpty()  &&  ! args.dryRun )
   {
      fail( tr( "--bundle is required for an export" ) );
      return 2;
   }

   // --max-scope caps the export, --include-<scope> asks for a record type;
   // without either the whole chain is exported
   QString scopeKey = args.scope;

   if ( ! scopeKey.isEmpty()  &&
        US_DataPub::scopeOfKey( scopeKey ) == US_DataPub::ScopeNone )
   {
      fail( tr( "\"%1\" is not a scope; one of: %2" ).arg( scopeKey )
            .arg( US_DataPub::scopeKeys().join( ", " ) ) );
      return 2;
   }

   if ( ! args.includeScope.isEmpty() )
   {
      if ( ! scopeKey.isEmpty()  &&
           US_DataPub::scopeOfKey( args.includeScope ) >
           US_DataPub::scopeOfKey( scopeKey ) )
      {
         fail( tr( "--include-%1 goes beyond --max-scope %2" )
               .arg( args.includeScope ).arg( scopeKey ) );
         return 2;
      }

      // Within the cap, what was asked for is what is exported
      scopeKey = args.includeScope;
   }

   if ( scopeKey.isEmpty() )
      scopeKey = US_DataPub::scopeKey( US_DataPub::ScopeNoise );

   US_DataPub::Scope scope = US_DataPub::scopeOfKey( scopeKey );

   if ( scope == US_DataPub::ScopeNone )
   {
      fail( tr( "\"%1\" is not a scope; one of: %2" ).arg( scopeKey )
            .arg( US_DataPub::scopeKeys().join( ", " ) ) );
      return 2;
   }

   if ( args.source != "db"  &&  args.source != "disk" )
   {
      fail( tr( "--source must be \"db\" or \"disk\"" ) );
      return 2;
   }

   if ( args.experiments.isEmpty()  &&  args.projectID.isEmpty()  &&
        args.projectGUID.isEmpty() )
   {
      fail( tr( "Nothing to export: give --experiment-id, --project-id or"
                " --project-guid" ) );
      return 2;
   }

   US_DataPubExporter::Selection selection;
   selection.fromDb           = ( args.source == "db" );
   int before                 = errors_reported;
   selection.dbPassword       = password( selection.fromDb );
   selection.scope            = scope;

   // One error for one problem: password() has said what went wrong when
   // it could.  A file or terminal that could not be read is a failure
   // (1); no password given at all is a usage error (2).
   if ( selection.fromDb  &&  selection.dbPassword.isEmpty() )
   {
      if ( errors_reported == before )
         fail( tr( "No database password; nothing was exported" ) );

      return password_failed ? 1 : 2;
   }

   selection.projectGUID      = args.projectGUID;
   selection.includeTimeState = args.timeState;
   selection.includeProject   = args.project;
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

   if ( ! args.projectID.isEmpty() )
   {
      QList< US_DataPubCatalog::Project > projects = catalog.projects( error );

      if ( ! error.isEmpty() )
      {
         fail( error );
         return 1;
      }

      QString guid;

      for ( int ii = 0; ii < projects.size(); ii++ )
      {
         if ( projects[ ii ].id != args.projectID )  continue;

         guid = projects[ ii ].guid;
         break;
      }

      if ( guid.isEmpty() )
      {
         fail( tr( "No project with ID %1 was found" ).arg( args.projectID ) );
         return 1;
      }

      if ( ! selection.projectGUID.isEmpty()  &&
           selection.projectGUID != guid )
      {
         fail( tr( "--project-id %1 and --project-guid %2 name different"
                   " projects" ).arg( args.projectID )
               .arg( selection.projectGUID ) );
         return 2;
      }

      selection.projectGUID = guid;
   }

   if ( ! args.experiments.isEmpty() )
   {
      QList< US_DataPubCatalog::Run > runs = catalog.runs( QString(), error );

      if ( ! error.isEmpty() )
      {
         fail( error );
         return 1;
      }

      for ( int ii = 0; ii < args.experiments.size(); ii++ )
      {
         QString wanted = args.experiments[ ii ];
         QString why;
         QString runID  = resolveRun( runs, wanted, why );

         if ( runID.isEmpty() )
         {
            fail( why );
            return 1;
         }

         if ( ! why.isEmpty() )
         {
            err << tr( "Warning: " ) << why << "\n";
            err.flush();
         }

         // A run named twice -- by runID and by ID, say -- is one run
         if ( ! selection.runIDs.contains( runID ) )
            selection.runIDs << runID;
      }
   }

   US_DataPubExporter exporter;
   connect( &exporter, &US_DataPubExporter::message,
            this,      &US_DataPubCLI::onMessage );

   if ( args.dryRun )
   {
      if ( ! exporter.previewManifest( selection, error ) )
      {
         fail( error );
         return 1;
      }

      print( args.bundle.isEmpty()
             ? tr( "Dry run: %1 records would be exported" )
               .arg( exporter.manifest().total() )
             : tr( "Dry run: %1 records would be exported to %2" )
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

   if ( ! args.listOnly  &&  args.target != "db"  &&  args.target != "disk" )
   {
      fail( tr( "--target must be \"db\" or \"disk\"" ) );
      return 2;
   }

   bool ok = true;
   US_DataPubImporter::Options options;
   options.policy = US_DataPub::policyOfKey( args.conflict, &ok );

   if ( ! ok )
   {
      fail( tr( "--on-conflict must be \"reuse\", \"rename\" or \"fail\"" ) );
      return 2;
   }

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

   // The password comes first: a database import that cannot have it
   // stops before the bundle is unpacked, with one error saying why
   if ( ! args.listOnly  &&  args.target == "db" )
   {
      int before = errors_reported;
      options.dbPassword = password( true );

      if ( options.dbPassword.isEmpty() )
      {
         // A password file that cannot be read is a failure, like any
         // other file the run cannot read; no password given at all is a
         // usage error
         if ( errors_reported != before  &&  ! args.dbPasswordFile.isEmpty() )
            return 1;

         if ( errors_reported == before )
            fail( tr( "A database import needs the master password: give"
                      " --db-password-file, or set US3_DB_PASSWORD" ) );
         return 2;
      }
   }

   US_DataPubImporter importer;
   connect( &importer, &US_DataPubImporter::message,
            this,      &US_DataPubCLI::onMessage );

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

   options.target         = ( args.target == "db" ) ? US_DataPub::TargetDb
                                                    : US_DataPub::TargetDisk;
   options.outputDir      = args.outputDir;
   options.projectGUID    = args.projectGUID;
   options.dryRun         = args.dryRun;
   options.verifyHashes   = args.verify;
   options.strictHardware = args.strictHardware;

   if ( ! args.renameSuffix.isEmpty() )
      options.renameSuffix = args.renameSuffix;

   bool imported = importer.runImport( options, error );

   QList< US_DataPubImporter::Result > results = importer.results();
   int counts[ 8 ] = { 0, 0, 0, 0, 0, 0, 0, 0 };

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      int res = int( results[ ii ].resolution );

      if ( res >= 0  &&  res < 8 )  counts[ res ]++;

      if ( ! verbose )  continue;

      print( QString( "  %1  %2 -> %3 (%4)" )
             .arg( US_DataPub::typeText( results[ ii ].type ) )
             .arg( results[ ii ].name )
             .arg( results[ ii ].targetName )
             .arg( US_DataPubImporter::resolutionText(
                   results[ ii ].resolution ) ) );
   }

   QStringList warnings = importer.warnings();

   for ( int ii = 0; ii < warnings.size(); ii++ )
   {
      err << tr( "Warning: " ) << warnings[ ii ] << "\n";
      err.flush();
   }

   if ( ! imported )
   {
      fail( error );

      // What the failure left behind: rolled back, or the files kept
      if ( ! importer.outcome().isEmpty() )
      {
         err << importer.outcome() << "\n";
         err.flush();
      }

      return 1;
   }

   QString tally = tr( "%1 created, %2 reused, %3 renamed, %4 substituted,"
                       " %5 not imported" )
                   .arg( counts[ US_DataPub::ResolvedCreated     ] )
                   .arg( counts[ US_DataPub::ResolvedReused      ] )
                   .arg( counts[ US_DataPub::ResolvedRenamed     ] )
                   .arg( counts[ US_DataPub::ResolvedSubstituted ] )
                   .arg( counts[ US_DataPub::ResolvedOmitted     ] );

   print( args.dryRun
          ? tr( "Dry run, nothing was written: %1 records would be %2" )
            .arg( results.size() ).arg( tally )
          : tr( "%1 records: %2" ).arg( results.size() ).arg( tally ) );

   if ( ! warnings.isEmpty() )
      print( tr( "%1 warnings; see the error output" ).arg( warnings.size() ) );

   return 0;
}
