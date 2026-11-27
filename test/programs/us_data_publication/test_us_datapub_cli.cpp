// test/programs/us_data_publication/test_us_datapub_cli.cpp
#include <gtest/gtest.h>

#include "datapub_test_env.h"

#include "us_datapub_cli.h"
#include "us_datapub_bundle.h"
#include "us_datapub_manifest.h"
#include "us_crypto.h"
#include "us_datapub_export.h"
#include "us_datapub_hash.h"

#if defined( Q_OS_UNIX )
#include <fcntl.h>
#include <unistd.h>
#endif

/**
 * The command line's export arguments, run against the scratch disk store.
 * Each case runs the front end exactly as main() does and looks at the exit
 * code and, where one was written, at the bundle.
 */
class DataPubCli : public DataPubTestEnv
{
   protected:
      QByteArray  savedPassword;
      bool        passwordSet = false;
      QStringList savedDB;
      QByteArray  savedHash;

      void SetUp() override
      {
         DataPubTestEnv::SetUp();

         // A password in the environment would answer what these cases ask
         passwordSet   = qEnvironmentVariableIsSet( "US3_DB_PASSWORD" );
         savedPassword = qgetenv( "US3_DB_PASSWORD" );
         qunsetenv( "US3_DB_PASSWORD" );

         savedDB   = US_Settings::defaultDB();
         savedHash = US_Settings::UltraScanPW();
      }

      void TearDown() override
      {
         if ( passwordSet )
            qputenv( "US3_DB_PASSWORD", savedPassword );
         else
            qunsetenv( "US3_DB_PASSWORD" );

         if ( sandboxed )
         {
            US_Settings::set_defaultDB   ( savedDB   );
            US_Settings::set_UltraScanPW( savedHash );
         }

         DataPubTestEnv::TearDown();
      }

      int run( const QStringList& args )
      {
         US_DataPubCLI cli;
         return cli.run( QStringList( "us_data_publication" ) + args );
      }

#if defined( Q_OS_UNIX )
      // Run, and collect what the command line wrote to its error output
      // -- and, with \a outText, to its standard output
      int runCapturing( const QStringList& args, QString& errText,
                        QString* outText = nullptr )
      {
         QString path  = root + "/stderr.txt";
         QString opath = root + "/stdout.txt";
         int     saved = dup( STDERR_FILENO );
         int     fd    = ::open( QFile::encodeName( path ).constData(),
                                 O_WRONLY | O_CREAT | O_TRUNC, 0600 );
         int     osaved = -1;
         fflush( stderr );
         dup2( fd, STDERR_FILENO );
         ::close( fd );

         if ( outText != nullptr )
         {
            osaved = dup( STDOUT_FILENO );
            fd     = ::open( QFile::encodeName( opath ).constData(),
                             O_WRONLY | O_CREAT | O_TRUNC, 0600 );
            fflush( stdout );
            dup2( fd, STDOUT_FILENO );
            ::close( fd );
         }

         int code;
         {
            US_DataPubCLI cli;
            code = cli.run( QStringList( "us_data_publication" ) + args );
         }

         fflush( stderr );
         dup2( saved, STDERR_FILENO );
         ::close( saved );

         if ( outText != nullptr )
         {
            fflush( stdout );
            dup2( osaved, STDOUT_FILENO );
            ::close( osaved );

            QFile ofile( opath );
            ofile.open( QIODevice::ReadOnly );
            *outText = QString::fromUtf8( ofile.readAll() );
         }

         QFile file( path );
         file.open( QIODevice::ReadOnly );
         errText = QString::fromUtf8( file.readAll() );

         return code;
      }
#endif

      // The manifest of a bundle the command line wrote
      bool manifestOf( const QString& bundlePath, US_DataPubManifest& mani )
      {
         US_DataPubBundle bundle;
         QString          error;

         if ( ! bundle.unpack( bundlePath, error ) )  return false;

         return mani.read( bundle.rootPath() + "/"
                           + US_DataPubBundle::manifestName(), error );
      }
};

TEST_F( DataPubCli, AnyLongOptionSelectsTheCommandLine )
{
   EXPECT_FALSE( US_DataPubCLI::wanted( QStringList( "prog" ) ) );
   EXPECT_FALSE( US_DataPubCLI::wanted( QStringList( "prog" ) << "-style"
                                                              << "fusion" ) );
   EXPECT_TRUE ( US_DataPubCLI::wanted( QStringList( "prog" ) << "--mode"
                                                              << "export" ) );

   // A command line without --mode is reported, not taken for the window
   EXPECT_TRUE ( US_DataPubCLI::wanted( QStringList( "prog" ) << "--bundle"
                                                              << "x.tar.gz" ) );
   EXPECT_EQ( run( QStringList() << "--bundle" << root + "/x.tar.gz" ), 2 );
}

TEST_F( DataPubCli, AnOptionIsNotTakenForTheValueOfAnother )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--bundle"
                                 << "--dry-run" << "--experiment-id"
                                 << runID ), 2 );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << "--dry-run" ), 2 );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--frobnicate" ),
              2 );
}

TEST_F( DataPubCli, AValueGivenWithAnEqualsSignIsTakenAsItIs )
{
   QString bundle = root + "/inline.tar.gz";

   EXPECT_EQ( run( QStringList() << "--mode=export" << "--experiment-id"
                                 << runID << "--comment=--not-an-option"
                                 << "--max-scope=rawData"
                                 << "--bundle=" + bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.comment, "--not-an-option" );
}

// --include-<scope> asks for that record type; it used to be lost against
// a default scope that was already the whole chain
TEST_F( DataPubCli, IncludeSetsTheScope )
{
   QString bundle = root + "/include.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--project-id"
                                 << "7" << "--include-edits"
                                 << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.scope, "edits" );
   EXPECT_EQ  ( mani.count( US_DataPub::EditedData ), 1 );
   EXPECT_EQ  ( mani.count( US_DataPub::Model ),      0 );
   EXPECT_FALSE( mani.isDeclared( US_DataPub::Model ) );
}

TEST_F( DataPubCli, IncludeBeyondTheMaximumIsRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "rawData"
                                 << "--include-models" << "--dry-run" ), 2 );
}

TEST_F( DataPubCli, WithoutAnyScopeTheWholeChainIsExported )
{
   QString bundle = root + "/whole.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.scope, "noise" );
   EXPECT_EQ  ( mani.count( US_DataPub::Noise ), 1 );
}

TEST_F( DataPubCli, ARunNamedTwiceIsExportedOnce )
{
   QString bundle = root + "/twice.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--experiment-id" << runID
                                 << "--max-scope" << "rawData"
                                 << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ  ( mani.count( US_DataPub::Experiment ), 1 );
}

TEST_F( DataPubCli, ADryRunNeedsNoBundle )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--dry-run" ), 0 );

   // ... but a real export does
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID ), 2 );
}

TEST_F( DataPubCli, AnExportOfNothingIsRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--bundle"
                                 << root + "/nothing.tar.gz" ), 2 );
   EXPECT_FALSE( QFile::exists( root + "/nothing.tar.gz" ) );
}

TEST_F( DataPubCli, AnUnknownGuidFailsTheExport )
{
   QString bundle = root + "/ghost.tar.gz";

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--model-guid"
                                 << US_Util::new_guid()
                                 << "--bundle" << bundle ), 1 );
   EXPECT_FALSE( QFile::exists( bundle ) );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--noise-guid"
                                 << US_Util::new_guid()
                                 << "--bundle" << bundle ), 1 );
   EXPECT_FALSE( QFile::exists( bundle ) );
}

TEST_F( DataPubCli, ARunOfAnotherProjectIsRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--project-guid"
                                 << US_Util::new_guid() << "--dry-run" ), 1 );
}

TEST_F( DataPubCli, ProjectIdAndGuidMustAgree )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--project-id"
                                 << "7" << "--project-guid"
                                 << US_Util::new_guid() << "--dry-run" ), 2 );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--project-id"
                                 << "7" << "--project-guid" << projectGUID
                                 << "--max-scope" << "rawData"
                                 << "--dry-run" ), 0 );
}

TEST_F( DataPubCli, ANonInteractiveRunWithoutAPasswordStops )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--source" << "db"
                                 << "--experiment-id" << runID
                                 << "--non-interactive" << "--dry-run" ), 2 );

}

// A password file that cannot be read is a failure, not a usage error, and
// it is reported once
TEST_F( DataPubCli, AnUnreadablePasswordFileFailsWithOneError )
{
#if defined( Q_OS_UNIX )
   QString errText;

   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--source" << "db" << "--experiment-id" << runID
                            << "--db-password-file" << root + "/no_such_file"
                            << "--dry-run", errText ), 1 );
   EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "no_such_file" ) ) << errText.toStdString();

   // A directory opens, but cannot be read
   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--source" << "db" << "--experiment-id" << runID
                            << "--db-password-file" << root
                            << "--dry-run", errText ), 1 );
   EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
#else
   GTEST_SKIP() << "the error output is captured on Unix only";
#endif
}

// An empty password was given, and is no use: that is a failure (1), as an
// unreadable password file is, not a usage error (2), and it is said once
TEST_F( DataPubCli, AnEmptyPasswordFailsWithOneError )
{
#if defined( Q_OS_UNIX )
   QString path = root + "/pw_empty.txt";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "\n" );
   file.close();
   QFile::setPermissions( path, QFileDevice::ReadOwner |
                                QFileDevice::WriteOwner );

   QString errText;

   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--source" << "db" << "--experiment-id" << runID
                            << "--db-password-file" << path
                            << "--non-interactive" << "--dry-run", errText ),
              1 );
   EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "empty password" ) ) << errText.toStdString();

   qputenv( "US3_DB_PASSWORD", "\r\n" );
   int code = runCapturing( QStringList() << "--mode" << "export"
                            << "--source" << "db" << "--experiment-id" << runID
                            << "--non-interactive" << "--dry-run", errText );
   qunsetenv( "US3_DB_PASSWORD" );

   EXPECT_EQ( code, 1 );
   EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "US3_DB_PASSWORD" ) ) << errText.toStdString();
#else
   GTEST_SKIP() << "the error output is captured on Unix only";
#endif
}

TEST_F( DataPubCli, APasswordFileThatIsADirectoryIsNotRead )
{
   QString pw, warning, error;

   EXPECT_FALSE( US_DataPubCLI::readPasswordFile( root, pw, warning, error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_TRUE ( pw.isEmpty() );
}

// --include-<scope> selects the scope also when --max-scope caps it
TEST_F( DataPubCli, IncludeCountsUnderAMaxScope )
{
   QString bundle = root + "/include.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "noise"
                                 << "--include-solutions"
                                 << "--bundle" << bundle ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ( mani.scope, QString( "solutions" ) );
   EXPECT_EQ( mani.count( US_DataPub::EditedData ), 0 );
   EXPECT_EQ( mani.count( US_DataPub::Model ), 0 );

   // Beyond the cap is still a usage error, and so is a cap that is no scope
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "rawData"
                                 << "--include-models" << "--dry-run" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--max-scope" << "everything"
                                 << "--include-models" << "--dry-run" ), 2 );
}

// A run ID made only of digits is the run ID, not another run's database ID
TEST_F( DataPubCli, AnExactRunIdWinsOverADatabaseId )
{
   QList< US_DataPubCatalog::Run > runs;
   US_DataPubCatalog::Run          one;
   one.runID = "12345";
   one.id    = "7";
   US_DataPubCatalog::Run          two;
   two.runID = "other_run";
   two.id    = "12345";
   runs << two << one;

   QString why;
   EXPECT_EQ( US_DataPubCLI::resolveRun( runs, "12345", why ),
              QString( "12345" ) );
   EXPECT_TRUE( why.contains( "other_run" ) ) << why.toStdString();

   EXPECT_EQ( US_DataPubCLI::resolveRun( runs, "7", why ), QString( "12345" ) );
   EXPECT_TRUE( why.isEmpty() );

   // An ID two runs share names neither
   US_DataPubCatalog::Run three;
   three.runID = "third_run";
   three.id    = "7";
   runs << three;
   EXPECT_TRUE( US_DataPubCLI::resolveRun( runs, "7", why ).isEmpty() );
   EXPECT_TRUE( why.contains( "third_run" ) ) << why.toStdString();

   EXPECT_TRUE( US_DataPubCLI::resolveRun( runs, "nothing", why ).isEmpty() );
   EXPECT_FALSE( why.isEmpty() );
}

// An option of the other mode would be ignored without a word
TEST_F( DataPubCli, OptionsOfTheOtherModeAreRefused )
{
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--target" << "disk"
                                 << "--dry-run" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--on-conflict-solutions"
                                 << "rename" << "--dry-run" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--strict-hardware"
                                 << "--dry-run" ), 2 );

   QString bundle = root + "/no_such_bundle.tar.gz";

   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--model-guid"
                                 << US_Util::new_guid() ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--include-models" ), 2 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--source" << "db" ), 2 );

   // What both modes take is taken by both
   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--experiment-id"
                                 << runID << "--project-guid" << projectGUID
                                 << "--max-scope" << "rawData"
                                 << "--dry-run" ), 0 );
   EXPECT_EQ( run( QStringList() << "--mode" << "import" << "--bundle"
                                 << bundle << "--target" << "disk" ), 1 );
}

// Only the line end is taken off a password; spaces belong to it
TEST_F( DataPubCli, APasswordKeepsEverythingButItsLineEnd )
{
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret\n" ),     "secret" );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret\r\n" ),   "secret" );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( " secret \n" ),   " secret " );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret\n\n" ),   "secret\n" );
   EXPECT_EQ( US_DataPubCLI::chopLineEnd( "secret" ),       "secret" );

   QString path = root + "/pw.txt";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( " pass word \r\nsecond line\n" );
   file.close();
   ASSERT_TRUE( QFile::setPermissions( path, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner ) );

   QString pw, warning, error;
   ASSERT_TRUE( US_DataPubCLI::readPasswordFile( path, pw, warning, error ) )
      << error.toStdString();
   EXPECT_EQ  ( pw, " pass word " );
   EXPECT_TRUE( warning.isEmpty() ) << warning.toStdString();

   EXPECT_FALSE( US_DataPubCLI::readPasswordFile( root + "/none", pw, warning,
                                                  error ) );
   EXPECT_FALSE( error.isEmpty() );
}

#if defined( Q_OS_UNIX )
TEST_F( DataPubCli, APasswordFileOthersCanReadIsWarnedAbout )
{
   QString path = root + "/pw_open.txt";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "secret\n" );
   file.close();
   ASSERT_TRUE( QFile::setPermissions( path, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner |
                                             QFileDevice::ReadGroup |
                                             QFileDevice::ReadOther ) );

   QString pw, warning, error;
   ASSERT_TRUE( US_DataPubCLI::readPasswordFile( path, pw, warning, error ) );
   EXPECT_EQ  ( pw, "secret" );
   EXPECT_TRUE( warning.contains( "other users" ) ) << warning.toStdString();
}
#endif

// A password from the file or the environment is taken: the run goes on to
// the database (and fails there, with no server) rather than stopping for
// want of a password
TEST_F( DataPubCli, APasswordFromAFileOrTheEnvironmentIsTaken )
{
   QString path = root + "/pw_ok.txt";
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "secret\n" );
   file.close();
   QFile::setPermissions( path, QFileDevice::ReadOwner |
                                QFileDevice::WriteOwner );

   EXPECT_EQ( run( QStringList() << "--mode" << "export" << "--source" << "db"
                                 << "--experiment-id" << runID
                                 << "--db-password-file" << path
                                 << "--non-interactive" << "--dry-run" ), 1 );

   qputenv( "US3_DB_PASSWORD", "secret\n" );
   int code = run( QStringList() << "--mode" << "export" << "--source" << "db"
                                 << "--experiment-id" << runID
                                 << "--non-interactive" << "--dry-run" );
   qunsetenv( "US3_DB_PASSWORD" );

   EXPECT_EQ( code, 1 );
}

#if defined( Q_OS_UNIX )
/* What a project export leaves out is said without --verbose: a "Warning:"
   line on the error output for each thing left out, and a count in the
   last line of the standard output.  A copied run directory that is left
   out does not take the original run's models and noise with it.
*/
TEST_F( DataPubCli, WhatIsLeftOutIsReportedWithoutVerbose )
{
   ASSERT_TRUE( copyRunDir( "A_copy_run", false ) );

   QString bundle = root + "/copy.tar.gz";
   QString errText;
   QString outText;

   ASSERT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--project-guid" << projectGUID
                            << "--non-interactive" << "--bundle" << bundle,
                            errText, &outText ), 0 )
      << errText.toStdString();

   EXPECT_TRUE( errText.contains( "Warning: run A_copy_run of the project is"
                                  " left out: " ) ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "Warning: 1 run(s) of the project are left"
                                  " out" ) ) << errText.toStdString();
   EXPECT_FALSE( errText.contains( "Error:" ) ) << errText.toStdString();
   EXPECT_TRUE( outText.contains( "Wrote " ) ) << outText.toStdString();
   EXPECT_TRUE( outText.contains( "(left out: 1 run(s), 0 other record(s);"
                                  " see the warnings)" ) )
      << outText.toStdString();

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( bundle, mani ) );
   EXPECT_EQ( mani.count( US_DataPub::Experiment ), 1 );
   EXPECT_TRUE( mani.contains( US_DataPub::Model, modelGUID ) );
   EXPECT_TRUE( mani.contains( US_DataPub::Noise, noiseGUID ) );

   // A dry run says the same -- and leaves out the same run: the copy,
   // which only its staged experiment XML shows to be one.  It used to
   // keep the copy, which comes first, and leave the original out.
   ASSERT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--project-guid" << projectGUID
                            << "--non-interactive" << "--dry-run",
                            errText, &outText ), 0 );
   EXPECT_TRUE( outText.contains( "left out: " ) ) << outText.toStdString();
   EXPECT_TRUE( outText.contains( QString( "Dry run: %1 records" )
                                  .arg( mani.total() ) ) )
      << outText.toStdString();
   EXPECT_TRUE( errText.contains( "Warning: run A_copy_run of the project is"
                                  " left out: " ) ) << errText.toStdString();
   EXPECT_FALSE( errText.contains( "run " + runID + " of the project is left"
                                   " out" ) ) << errText.toStdString();

   // Nothing left out, nothing said
   QDir( US_Settings::resultDir() + "/A_copy_run" ).removeRecursively();

   ASSERT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--project-guid" << projectGUID
                            << "--non-interactive" << "--bundle"
                            << root + "/whole.tar.gz", errText, &outText ),
              0 );
   EXPECT_FALSE( errText.contains( "left out" ) ) << errText.toStdString();
   EXPECT_FALSE( outText.contains( "left out" ) ) << outText.toStdString();
}

/* A model or noise record asked for by GUID whose run a project export
   leaves out fails the export (exit 1), saying why, rather than vanishing
   from a bundle that is written all the same.
*/
TEST_F( DataPubCli, AModelOrNoiseAskedForWhoseRunIsLeftOutFailsTheExport )
{
   // A second run of the project, of its own, which holds together ...
   QString runID1   = runID;
   QString expGUID1 = expGUID;
   QString rawGUID1 = rawGUID;

   runID   = "demo_run2";
   expGUID = US_Util::new_guid();
   rawGUID = US_Util::new_guid();
   writeRawFile( 1, rawGUID );
   writeExperimentXml();

   runID   = runID1;
   expGUID = expGUID1;
   rawGUID = rawGUID1;

   // ... and the first, with the model and noise, which does not
   ASSERT_TRUE( set_xml_attribute( runDir() + "/" + runID + "." + runType
                                   + ".xml", "experiment", "runID",
                                   "old_name" ) );

   QStringList base = QStringList() << "--mode" << "export"
                                    << "--project-guid" << projectGUID
                                    << "--non-interactive";
   QString     errText;
   QString     outText;

   ASSERT_EQ( runCapturing( base + ( QStringList() << "--bundle"
                                     << root + "/plain.tar.gz" ),
                            errText, &outText ), 0 ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "Warning: model " ) ) << errText.toStdString();
   EXPECT_TRUE( outText.contains( "(left out: 1 run(s), 2 other record(s)" ) )
      << outText.toStdString();

   QString bundle = root + "/model.tar.gz";

   EXPECT_EQ( runCapturing( base + ( QStringList() << "--model-guid"
                                     << modelGUID << "--bundle" << bundle ),
                            errText ), 1 );
   EXPECT_TRUE( errText.contains( "Error: " ) ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( modelGUID ) ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "left out" ) ) << errText.toStdString();
   EXPECT_FALSE( QFile::exists( bundle ) );

   EXPECT_EQ( runCapturing( base + ( QStringList() << "--noise-guid"
                                     << noiseGUID << "--bundle" << bundle ),
                            errText ), 1 );
   EXPECT_TRUE( errText.contains( noiseGUID ) ) << errText.toStdString();
   EXPECT_FALSE( QFile::exists( bundle ) );
}

// A record holding a value that is not a finite number is not exported
TEST_F( DataPubCli, ANonFiniteValueFailsTheExport )
{
   QString dir  = dataDir( "buffers" );
   QString file = dir + "/" + QDir( dir ).entryList( QDir::Files )[ 0 ];
   ASSERT_TRUE( set_xml_attribute( file, "buffer", "density", "nan" ) );

   QString errText;
   QString bundle = root + "/nan.tar.gz";

   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--experiment-id" << runID
                            << "--non-interactive" << "--bundle" << bundle,
                            errText ), 1 );
   EXPECT_TRUE( errText.contains( "density=\"nan\"" ) ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "not a finite number" ) )
      << errText.toStdString();
   EXPECT_FALSE( QFile::exists( bundle ) );
}

/* US3_DB_PASSWORD set to nothing is a password given, and an empty one: a
   failure (1) said once, as for an empty password file -- not a usage error
   (2), and no prompt either.  It used to be taken for no password at all.
*/
TEST_F( DataPubCli, AnEmptyPasswordInTheEnvironmentFailsWithOneError )
{
   qputenv( "US3_DB_PASSWORD", QByteArray() );
   ASSERT_TRUE( qEnvironmentVariableIsSet( "US3_DB_PASSWORD" ) );

   const QStringList modes[] =
   {
      { "--mode", "export", "--source", "db", "--experiment-id", "demo_run",
        "--dry-run", "--non-interactive" },
      { "--mode", "export", "--source", "db", "--experiment-id", "demo_run",
        "--dry-run" },
      { "--mode", "import", "--target", "db", "--bundle",
        "/nonexistent/bundle.tar.gz", "--non-interactive" }
   };

   for ( const QStringList& args : modes )
   {
      SCOPED_TRACE( args.join( " " ).toStdString() );
      QString errText;

      EXPECT_EQ( runCapturing( args, errText ), 1 );
      EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
      EXPECT_TRUE( errText.contains( "US3_DB_PASSWORD is set but holds an"
                                     " empty password" ) )
         << errText.toStdString();
      EXPECT_FALSE( errText.contains( "master password:" ) )
         << errText.toStdString();
   }
}

/* An empty --db-password is a password given, and an empty one, as an
   empty US3_DB_PASSWORD is: a failure (1) said once, not a usage error (2)
   with the whole help text.  Both spellings of the option.
*/
TEST_F( DataPubCli, AnEmptyPasswordOptionFailsAsTheEnvironmentDoes )
{
   const QStringList modes[] =
   {
      { "--mode", "export", "--source", "db", "--experiment-id", "demo_run",
        "--dry-run", "--non-interactive", "--db-password", "" },
      { "--mode", "export", "--source", "db", "--experiment-id", "demo_run",
        "--dry-run", "--db-password=" },
      { "--mode", "import", "--target", "db", "--bundle",
        "/nonexistent/bundle.tar.gz", "--non-interactive",
        "--db-password", "" }
   };

   for ( const QStringList& args : modes )
   {
      SCOPED_TRACE( args.join( " " ).toStdString() );
      QString errText;

      EXPECT_EQ( runCapturing( args, errText ), 1 );
      EXPECT_EQ( errText.count( "Error:" ), 1 ) << errText.toStdString();
      EXPECT_TRUE( errText.contains( "--db-password was given an empty"
                                     " password" ) )
         << errText.toStdString();
      EXPECT_FALSE( errText.contains( "Usage" ) ) << errText.toStdString();
      EXPECT_FALSE( errText.contains( "master password:" ) )
         << errText.toStdString();
   }

   // An option with no value at all is still a usage error
   QString errText;
   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export" << "--source"
                            << "db" << "--experiment-id" << "demo_run"
                            << "--db-password", errText ), 2 );
}

/* A wrong master password is said to be wrong.  It unlocks nothing, and the
   server then refused a login without any password: "Access denied ...
   (using password: NO)", which named neither the master password nor what
   to do about it.
*/
TEST_F( DataPubCli, AWrongMasterPasswordIsSaidToBeWrong )
{
   QStringList cipher = US_Crypto::encrypt( "dbpw", "right master" );
   QStringList entry;
   entry << "a database" << "us3" << "us3_somewhere" << "127.0.0.1:9"
         << cipher.at( 0 ) << cipher.at( 1 ) << "me@example.org"
         << cipher.at( 0 ) << cipher.at( 1 ) << US_Util::new_guid();
   US_Settings::set_defaultDB( entry );
   US_Settings::set_UltraScanPW( QByteArray() );

   EXPECT_TRUE ( US_DataPub::masterPasswordProblem( "right master" )
                 .isEmpty() );
   EXPECT_FALSE( US_DataPub::masterPasswordProblem( "wrong" ).isEmpty() );

   qputenv( "US3_DB_PASSWORD", "wrong" );
   QString errText;

   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--source" << "db" << "--experiment-id" << runID
                            << "--dry-run" << "--non-interactive", errText ),
              1 );
   EXPECT_TRUE( errText.contains( "master password is wrong" ) )
      << errText.toStdString();
   EXPECT_TRUE( errText.contains( "us3_somewhere" ) ) << errText.toStdString();
   EXPECT_FALSE( errText.contains( "using password" ) )
      << errText.toStdString();

   // Against the hash the installation keeps, when it keeps one
   US_Settings::set_UltraScanPW( QCryptographicHash::hash(
                                 QByteArray( "right master" ),
                                 QCryptographicHash::Sha1 ) );
   EXPECT_TRUE( US_DataPub::masterPasswordProblem( "wrong" )
                .contains( "not the one set" ) );
   EXPECT_TRUE( US_DataPub::masterPasswordProblem( "right master" )
                .isEmpty() );
}
#endif

/* An experiment ID picks one of several experiments that share a run ID;
   a run ID picks none of them (the export then says which IDs there are).
*/
TEST_F( DataPubCli, AnExperimentIdPicksOneOfTheExperimentsOfARunId )
{
   QList< US_DataPubCatalog::Run > runs;
   US_DataPubCatalog::Run          first;
   first.runID = "dup_run";
   first.id    = "12";
   US_DataPubCatalog::Run          second;
   second.runID = "dup_run";
   second.id    = "15";
   runs << first << second;

   QString why;
   QString expID = "stale";
   EXPECT_EQ( US_DataPubCLI::resolveRun( runs, "15", why, &expID ),
              QString( "dup_run" ) );
   EXPECT_EQ( expID, QString( "15" ) );
   EXPECT_TRUE( why.isEmpty() ) << why.toStdString();

   EXPECT_EQ( US_DataPubCLI::resolveRun( runs, "dup_run", why, &expID ),
              QString( "dup_run" ) );
   EXPECT_TRUE( expID.isEmpty() );
}

#if defined( Q_OS_UNIX )
/* --list shows what a bundle the import refuses holds -- here one of the
   format 1 bundles that listed borrowed noise under the borrower -- and
   then says why it is refused (exit 1).
*/
TEST_F( DataPubCli, ListShowsTheManifestOfARefusedBundle )
{
   QString bundle = root + "/plain.tar.gz";
   QString errText;
   QString outText;

   ASSERT_EQ( runCapturing( QStringList() << "--mode" << "export"
                            << "--experiment-id" << runID
                            << "--non-interactive" << "--bundle" << bundle,
                            errText, &outText ), 0 )
      << errText.toStdString();

   // The same bundle, as such a build wrote it
   US_DataPubBundle in;
   QString          error;
   ASSERT_TRUE( in.unpack( bundle, error ) ) << error.toStdString();

   US_DataPubBundle out;
   ASSERT_TRUE( out.createStaging( error ) ) << error.toStdString();

   QDir         src( in.rootPath() );
   QDirIterator it( in.rootPath(), QDir::Files, QDirIterator::Subdirectories );

   while ( it.hasNext() )
   {
      QString rel = src.relativeFilePath( it.next() );
      QDir().mkpath( QFileInfo( out.stagingPath() + "/" + rel ).path() );
      ASSERT_TRUE( QFile::copy( in.rootPath() + "/" + rel,
                                out.stagingPath() + "/" + rel ) );
   }

   QString            mpath = out.stagingPath() + "/"
                              + US_DataPubBundle::manifestName();
   US_DataPubManifest mani;
   ASSERT_TRUE( mani.read( mpath, error ) ) << error.toStdString();

   QString fitted = "aaaaaaaa-0000-4000-8000-0000000000f1";
   QList< US_DataPubEntity > noises = mani.section( US_DataPub::Noise );
   ASSERT_FALSE( noises.isEmpty() );
   noises[ 0 ].attrs.insert( "fittedToModelGUID", fitted );
   US_DataPubEntity noise = noises[ 0 ];
   mani.version = 1;
   mani.setSection( US_DataPub::Noise, noises );
   ASSERT_TRUE( mani.write( mpath, error ) ) << error.toStdString();

   QFile digest( out.stagingPath() + "/"
                 + US_DataPubExporter::manifestDigestName() );
   ASSERT_TRUE( digest.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
   digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                   + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
   digest.close();

   QString refused = root + "/refused.tar.gz";
   ASSERT_TRUE( out.pack( refused, error ) ) << error.toStdString();

   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "import"
                            << "--bundle" << refused << "--list",
                            errText, &outText ), 1 );
   EXPECT_TRUE( outText.contains( "fittedToModelGUID" ) )
      << outText.toStdString();
   EXPECT_TRUE( outText.contains( noise.guid ) ) << outText.toStdString();
   EXPECT_TRUE( errText.contains( "Error:" ) ) << errText.toStdString();
   EXPECT_TRUE( errText.contains( "development build" ) )
      << errText.toStdString();

   // A bundle that is no bundle at all still shows nothing
   QFile junk( root + "/junk.tar.gz" );
   ASSERT_TRUE( junk.open( QIODevice::WriteOnly ) );
   junk.write( "not a bundle" );
   junk.close();
   EXPECT_EQ( runCapturing( QStringList() << "--mode" << "import"
                            << "--bundle" << junk.fileName() << "--list",
                            errText, &outText ), 1 );
   EXPECT_TRUE( outText.trimmed().isEmpty() ) << outText.toStdString();
}
#endif

/* A subset picked with --model-guid carries, for each model and noise
   record, where it stands in the source: sourceLastUpdated and
   sourceIndex (the file number on disk), which the import writes the
   records of an edit by -- the same as the whole run's bundle says.  A
   bundle without them fails.  (A model the runs do not list is looked up
   by itself, completeModel(); a local store lists every model of an edit,
   so the database case, AModelTheChainDoesNotListBorrowsTheNoiseOfItsEdit,
   covers that.)
*/
TEST_F( DataPubCli, ModelsPickedByGuidCarryTheirSourceOrder )
{
   QString whole  = root + "/whole.tar.gz";
   QString subset = root + "/subset.tar.gz";

   ASSERT_EQ( run( QStringList() << "--mode" << "export"
                   << "--experiment-id" << runID << "--non-interactive"
                   << "--bundle" << whole ), 0 );

   US_DataPubManifest all;
   ASSERT_TRUE( manifestOf( whole, all ) );
   QList< US_DataPubEntity > listed = all.section( US_DataPub::Model )
                                      + all.section( US_DataPub::Noise );
   ASSERT_EQ( listed.size(), 2 );

   for ( const US_DataPubEntity& record : listed )
   {
      EXPECT_TRUE( record.attrs.value( "sourceLastUpdated" ).endsWith( "Z" ) )
         << record.guid.toStdString();
      EXPECT_EQ( record.attrs.value( "sourceIndex" ), QString( "1" ) )
         << record.guid.toStdString();
   }

   ASSERT_EQ( run( QStringList() << "--mode" << "export"
                   << "--experiment-id" << runID << "--model-guid"
                   << modelGUID << "--non-interactive"
                   << "--bundle" << subset ), 0 );

   US_DataPubManifest mani;
   ASSERT_TRUE( manifestOf( subset, mani ) );

   QList< US_DataPubEntity > records = mani.section( US_DataPub::Model )
                                       + mani.section( US_DataPub::Noise );
   ASSERT_EQ( records.size(), 2 );

   for ( const US_DataPubEntity& record : records )
   {
      US_DataPubEntity inWhole;
      ASSERT_TRUE( all.entity( record.type, record.guid, inWhole ) )
         << record.guid.toStdString();
      EXPECT_EQ( record.attrs.value( "sourceLastUpdated" ),
                 inWhole.attrs.value( "sourceLastUpdated" ) )
         << record.guid.toStdString();
      EXPECT_EQ( record.attrs.value( "sourceIndex" ),
                 inWhole.attrs.value( "sourceIndex" ) )
         << record.guid.toStdString();
   }
}
