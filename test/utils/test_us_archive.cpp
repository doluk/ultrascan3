// test_us_archive.cpp - US_Archive treats an archive as untrusted input
//
// The archives are built here with libarchive itself, entry by entry, so a
// test can put into one exactly the entry a hostile archive would carry.
#include "qt_test_base.h"

#include <QtCore>
#include <QSignalSpy>

#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/resource.h>
#endif

#include "archive.h"
#include "archive_entry.h"

#include "us_archive.h"

namespace
{
   // One entry of a crafted archive
   struct Entry
   {
      QString    name;
      QByteArray data;
      int        type     = AE_IFREG;
      QString    linkTo;              // a symbolic link's target
      QString    hardTo;              // a hard link's target
      qint64     declared = -1;       // the size the header claims, if not data's
   };

   Entry file( const QString& name, const QByteArray& data )
   {
      Entry entry;
      entry.name = name;
      entry.data = data;
      return entry;
   }
}

class TestUSArchive : public QtTestBase
{
   protected:
      QTemporaryDir* scratch = nullptr;
      QString        root;

      void SetUp() override
      {
         QtTestBase::SetUp();
         scratch = new QTemporaryDir();
         ASSERT_TRUE( scratch->isValid() );
         root = scratch->path();
      }

      void TearDown() override
      {
         delete scratch;
         scratch = nullptr;
         QtTestBase::TearDown();
      }

      // Write a tar archive holding exactly the given entries
      QString makeTar( const QString& name, const QList< Entry >& entries )
      {
         QString path = root + "/" + name;
         struct archive* out = archive_write_new();
         archive_write_set_format_pax_restricted( out );
         EXPECT_EQ( archive_write_open_filename( out, path.toUtf8().constData() ),
                    ARCHIVE_OK );

         for ( const Entry& ee : entries )
         {
            struct archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname( entry, ee.name.toUtf8().constData() );
            archive_entry_set_filetype( entry, ee.type );
            archive_entry_set_perm    ( entry, 0644 );

            if ( ! ee.linkTo.isEmpty() )
               archive_entry_set_symlink( entry, ee.linkTo.toUtf8().constData() );

            if ( ! ee.hardTo.isEmpty() )
               archive_entry_set_hardlink( entry, ee.hardTo.toUtf8().constData() );

            archive_entry_set_size( entry, ee.type == AE_IFREG
                                    ? ( ee.declared >= 0 ? ee.declared : ee.data.size() )
                                    : 0 );
            EXPECT_GE( archive_write_header( out, entry ), ARCHIVE_WARN );

            if ( ! ee.data.isEmpty() )
               archive_write_data( out, ee.data.constData(), ee.data.size() );

            archive_entry_free( entry );
         }

         archive_write_close( out );
         archive_write_free ( out );
         return path;
      }

      // Everything below a directory, relative to it
      QStringList contentsOf( const QString& dir )
      {
         QStringList   found;
         QDirIterator  it( dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System
                                | QDir::Hidden, QDirIterator::Subdirectories );

         while ( it.hasNext() )
            found << QDir( dir ).relativeFilePath( it.next() );

         found.sort();
         return found;
      }

      // Extract a crafted archive and expect it refused with nothing written
      void expectRefused( const QList< Entry >& entries, const QString& why )
      {
         QString    tar  = makeTar( "crafted.tar", entries );
         QString    dest = root + "/out";
         US_Archive archive;
         QStringList files;

         EXPECT_FALSE( archive.extract( tar, dest, &files ) ) << why.toStdString();
         EXPECT_FALSE( archive.getError().isEmpty() );
         EXPECT_TRUE ( files.isEmpty() );

         // Nothing at all is written, inside the destination or next to it
         EXPECT_TRUE( contentsOf( dest ).isEmpty() )
            << contentsOf( dest ).join( ", " ).toStdString();
         EXPECT_FALSE( QFile::exists( root + "/evil.txt" ) ) << why.toStdString();
      }
};

TEST_F( TestUSArchive, AnOrdinaryArchiveIsExtracted )
{
   Entry dir;
   dir.name = "run";
   dir.type = AE_IFDIR;

   QString tar = makeTar( "good.tar", QList< Entry >()
                          << dir
                          << file( "run/a.auc", "alpha" )
                          << file( "run/sub/b.xml", "beta" ) );

   US_Archive  archive;
   QStringList files;

   ASSERT_TRUE( archive.extract( tar, root + "/out", &files ) )
      << archive.getError().toStdString();
   EXPECT_EQ( files.size(), 2 );

   QFile ff( root + "/out/run/sub/b.xml" );
   ASSERT_TRUE( ff.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( ff.readAll(), QByteArray( "beta" ) );
}

TEST_F( TestUSArchive, ParentDirectoryComponentsAreRefused )
{
   expectRefused( QList< Entry >() << file( "ok.txt", "x" )
                                   << file( "../evil.txt", "x" ),
                  "a leading .." );
   expectRefused( QList< Entry >() << file( "a/../../evil.txt", "x" ),
                  "a .. in the middle" );
   expectRefused( QList< Entry >() << file( "a\\..\\..\\evil.txt", "x" ),
                  "a .. behind backslashes" );
}

TEST_F( TestUSArchive, AbsolutePathsAndDriveLettersAreRefused )
{
   expectRefused( QList< Entry >() << file( root + "/evil.txt", "x" ),
                  "an absolute path" );
   expectRefused( QList< Entry >() << file( "C:/evil.txt", "x" ),
                  "a drive letter" );
   expectRefused( QList< Entry >() << file( "\\evil.txt", "x" ),
                  "a rooted backslash path" );
}

TEST_F( TestUSArchive, LinksAndSpecialFilesAreRefused )
{
   Entry symlink;
   symlink.name   = "link";
   symlink.type   = AE_IFLNK;
   symlink.linkTo = root + "/evil.txt";
   expectRefused( QList< Entry >() << symlink, "a symbolic link" );

   Entry hardlink;
   hardlink.name   = "hard";
   hardlink.hardTo = "../evil.txt";
   expectRefused( QList< Entry >() << file( "target", "x" ) << hardlink,
                  "a hard link" );

   Entry fifo;
   fifo.name = "pipe";
   fifo.type = AE_IFIFO;
   expectRefused( QList< Entry >() << fifo, "a FIFO" );

   Entry device;
   device.name = "dev";
   device.type = AE_IFCHR;
   expectRefused( QList< Entry >() << device, "a device" );
}

TEST_F( TestUSArchive, EntriesOverTheSizeLimitAreRefused )
{
   QString    tar  = makeTar( "big.tar", QList< Entry >()
                              << file( "small.txt", QByteArray( 10, 'a' ) )
                              << file( "large.txt", QByteArray( 4096, 'b' ) ) );
   US_Archive archive;

   archive.setLimits( 1000, 1000000 );
   EXPECT_FALSE( archive.extract( tar, root + "/out" ) );
   EXPECT_TRUE ( archive.getError().contains( "large.txt" ) )
      << archive.getError().toStdString();
   EXPECT_TRUE ( contentsOf( root + "/out" ).isEmpty() );

   // the same archive under a total limit it exceeds only in sum
   archive.setLimits( 5000, 4100 );
   EXPECT_FALSE( archive.extract( tar, root + "/out2" ) );
   EXPECT_TRUE ( contentsOf( root + "/out2" ).isEmpty() );

   // and within the limits it is fine
   archive.setLimits( 5000, 5000 );
   EXPECT_TRUE( archive.extract( tar, root + "/out3" ) )
      << archive.getError().toStdString();
}

// A small compressed file that inflates to far more than it is, refused
// before a byte of it is written.  (The limit is applied a second time to
// the bytes actually decompressed, for formats whose header can leave the
// size out; libarchive's own writers always record it, so that second
// check is not reachable from an archive built here.)
TEST_F( TestUSArchive, ACompressionBombIsRefusedBeforeAnythingIsWritten )
{
   QString source = root + "/zeros.bin";
   QFile   ff( source );
   ASSERT_TRUE( ff.open( QIODevice::WriteOnly ) );
   ff.write( QByteArray( 1 << 20, '\0' ) );
   ff.close();

   QString    tgz = root + "/bomb.tar.gz";
   US_Archive archive;
   ASSERT_TRUE( archive.compress( QStringList() << source, tgz ) )
      << archive.getError().toStdString();
   EXPECT_LT( QFileInfo( tgz ).size(), 100000 );

   archive.setLimits( 1000, 1000 );
   EXPECT_FALSE( archive.extract( tgz, root + "/out" ) );
   EXPECT_FALSE( QFile::exists( root + "/out/zeros.bin" ) );
}

TEST_F( TestUSArchive, SafeEntryNamesAreRecognised )
{
   EXPECT_TRUE ( US_Archive::safeEntryName( "a.txt" ) );
   EXPECT_TRUE ( US_Archive::safeEntryName( "dir/sub/a..b.txt" ) );
   EXPECT_TRUE ( US_Archive::safeEntryName( "./a.txt" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "/etc/passwd" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "c:evil" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "a/../../b" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( ".." ) );
}

TEST_F( TestUSArchive, ARoundTripKeepsTheContents )
{
   QDir().mkpath( root + "/src/sub" );
   QFile aa( root + "/src/sub/a.txt" );
   ASSERT_TRUE( aa.open( QIODevice::WriteOnly ) );
   aa.write( "contents of a" );
   aa.close();

   QString    zip = root + "/round.zip";
   US_Archive archive;
   ASSERT_TRUE( archive.compress( QStringList() << root + "/src", zip ) )
      << archive.getError().toStdString();
   ASSERT_TRUE( archive.extract( zip, root + "/back" ) )
      << archive.getError().toStdString();

   QFile back( root + "/back/src/sub/a.txt" );
   ASSERT_TRUE( back.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( back.readAll(), QByteArray( "contents of a" ) );
}

// A write that fails part way must not be reported as an archive.  An
// archive named *.tar that is really /dev/full accepts the open and fails
// every write, which is the failure compress() used to ignore.
TEST_F( TestUSArchive, AFailedWriteFailsTheCompression )
{
#ifdef Q_OS_LINUX
   if ( ! QFile::exists( "/dev/full" ) )
      GTEST_SKIP() << "no /dev/full";

   QString source = root + "/data.bin";
   QFile   ff( source );
   ASSERT_TRUE( ff.open( QIODevice::WriteOnly ) );
   ff.write( QByteArray( 300000, 'x' ) );
   ff.close();

   QString full = root + "/full.tar";
   ASSERT_TRUE( QFile::link( "/dev/full", full ) );

   US_Archive archive;
   EXPECT_FALSE( archive.compress( QStringList() << source, full ) );
   EXPECT_FALSE( archive.getError().isEmpty() );
#else
   GTEST_SKIP() << "needs /dev/full";
#endif
}

// A truncated archive is a damaged one, not a short success
TEST_F( TestUSArchive, ATruncatedArchiveIsAnError )
{
   QString source = root + "/data.bin";
   QFile   ff( source );
   ASSERT_TRUE( ff.open( QIODevice::WriteOnly ) );
   for ( int ii = 0; ii < 20000; ii++ )
      ff.write( QByteArray::number( ii * 7919 ) );
   ff.close();

   QString    tgz = root + "/cut.tar.gz";
   US_Archive archive;
   ASSERT_TRUE( archive.compress( QStringList() << source, tgz ) );

   QFile cut( tgz );
   ASSERT_TRUE( cut.open( QIODevice::ReadWrite ) );
   ASSERT_TRUE( cut.resize( cut.size() / 2 ) );
   cut.close();

   EXPECT_FALSE( archive.extract( tgz, root + "/out" ) );
   EXPECT_FALSE( archive.getError().isEmpty() );
}

// Two entries that would be written to one file are refused: the same name
// twice, or names a case-insensitive file system does not tell apart
TEST_F( TestUSArchive, DuplicateAndCaseCollidingNamesAreRefused )
{
   expectRefused( QList< Entry >() << file( "a.txt", "x" ) << file( "a.txt", "y" ),
                  "the same name twice" );
   expectRefused( QList< Entry >() << file( "a.txt", "x" ) << file( "./a.txt", "y" ),
                  "the same name spelled two ways" );
   expectRefused( QList< Entry >() << file( "A.txt", "x" ) << file( "a.txt", "y" ),
                  "names that differ only in case" );
   expectRefused( QList< Entry >() << file( "Run/x.auc", "x" ) << file( "run/y.auc", "y" ),
                  "directories that differ only in case" );
   expectRefused( QList< Entry >() << file( "a", "x" ) << file( "a/b", "y" ),
                  "a file that is also a directory" );

   // one accented name, composed and decomposed: macOS writes both to one file
   const QString composed   = QString( "caf" ) + QChar( 0x00e9 ) + ".txt";
   const QString decomposed = QString( "cafe" ) + QChar( 0x0301 ) + ".txt";
   expectRefused( QList< Entry >() << file( composed, "x" ) << file( decomposed, "y" ),
                  "names that differ only in normalization" );
   expectRefused( QList< Entry >() << file( "Caf" + QString( QChar( 0x00e9 ) ) + "/a", "x" )
                                   << file( decomposed.left( 5 ) + "/b", "y" ),
                  "directories that differ in case and normalization" );

   // a directory listed again, and files below it, are fine
   Entry dir;
   dir.name = "run";
   dir.type = AE_IFDIR;
   QString tar = makeTar( "dirs.tar", QList< Entry >() << dir << dir
                          << file( "run/a.auc", "a" ) << file( "run/b.auc", "b" ) );
   US_Archive archive;
   EXPECT_TRUE( archive.extract( tar, root + "/fine" ) ) << archive.getError().toStdString();
}

// Names Windows cannot hold are refused, so an archive extracts the same
// everywhere
TEST_F( TestUSArchive, NamesWindowsCannotHoldAreRefused )
{
   expectRefused( QList< Entry >() << file( "a:b.txt", "x" ), "a colon" );
   expectRefused( QList< Entry >() << file( "dir./a.txt", "x" ), "a trailing dot" );
   expectRefused( QList< Entry >() << file( "dir /a.txt", "x" ), "a trailing space" );
   expectRefused( QList< Entry >() << file( "CON", "x" ), "a device name" );
   expectRefused( QList< Entry >() << file( "sub/nul.txt", "x" ),
                  "a device name with an extension" );
   expectRefused( QList< Entry >() << file( "com1/a.auc", "x" ), "a device directory" );

   // refused before anything is written, the good entry before it included
   expectRefused( QList< Entry >() << file( "ok.txt", "x" ) << file( "a<b>.txt", "x" ),
                  "angle brackets" );
   expectRefused( QList< Entry >() << file( "ok.txt", "x" ) << file( "run/what?.auc", "x" ),
                  "a question mark" );
   expectRefused( QList< Entry >() << file( "ok.txt", "x" ) << file( "a\001b", "x" ),
                  "a control character" );

   EXPECT_FALSE( US_Archive::safeEntryName( "Lpt9" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "aux .txt" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "./" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "a?b.txt" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "run/a*.auc" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "a|b" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( "quote\"d" ) );
   EXPECT_FALSE( US_Archive::safeEntryName( QString( "tab\there" ) ) );
   EXPECT_FALSE( US_Archive::safeEntryName( QString( "bell" ) + QChar( 7 ) ) );
   EXPECT_TRUE ( US_Archive::safeEntryName( "CONSOLE.txt" ) );
   EXPECT_TRUE ( US_Archive::safeEntryName( "COM10/a.auc" ) );
   EXPECT_TRUE ( US_Archive::safeEntryName( "run/.hidden" ) );
}

// A link already in the destination is not written through
TEST_F( TestUSArchive, ALinkInTheDestinationIsNotFollowed )
{
#ifdef Q_OS_UNIX
   QString outside = root + "/outside";
   QString dest    = root + "/out";
   QDir().mkpath( outside );
   QDir().mkpath( dest );

   // a directory of the archive is a link out of the destination
   ASSERT_TRUE( QFile::link( outside, dest + "/run" ) );

   QString    tar = makeTar( "via-dir.tar", QList< Entry >() << file( "run/evil.txt", "x" ) );
   US_Archive archive;
   EXPECT_FALSE( archive.extract( tar, dest ) );
   EXPECT_TRUE ( archive.getError().contains( "link" ) ) << archive.getError().toStdString();
   EXPECT_FALSE( QFile::exists( outside + "/evil.txt" ) );

   // a file of the archive is a link to a file outside
   QFile::remove( dest + "/run" );
   ASSERT_TRUE( QFile::link( outside + "/target.txt", dest + "/a.txt" ) );

   tar = makeTar( "via-file.tar", QList< Entry >() << file( "a.txt", "x" ) );
   EXPECT_FALSE( archive.extract( tar, dest ) );
   EXPECT_FALSE( QFile::exists( outside + "/target.txt" ) );
#else
   GTEST_SKIP() << "needs symbolic links";
#endif
}

// Programs that always passed over links can still do so, and are told
TEST_F( TestUSArchive, LinksCanBeSkippedWithANote )
{
   Entry symlink;
   symlink.name   = "link";
   symlink.type   = AE_IFLNK;
   symlink.linkTo = root + "/evil.txt";

   Entry fifo;
   fifo.name = "pipe";
   fifo.type = AE_IFIFO;

   QString tar = makeTar( "links.tar", QList< Entry >()
                          << file( "a.txt", "alpha" ) << symlink << fifo );

   US_Archive archive;
   EXPECT_FALSE( archive.skipLinks() );
   archive.setSkipLinks( true );

   QSignalSpy  spy( &archive, &US_Archive::itemSkipped );
   QStringList files;

   ASSERT_TRUE( archive.extract( tar, root + "/out", &files ) )
      << archive.getError().toStdString();
   EXPECT_EQ( files.size(), 1 );
   EXPECT_EQ( contentsOf( root + "/out" ), QStringList() << "a.txt" );
   EXPECT_EQ( spy.count(), 2 );
   ASSERT_EQ( archive.skippedEntries().size(), 2 );
   EXPECT_TRUE( archive.skippedEntries()[ 0 ].startsWith( "link: " ) );
   EXPECT_TRUE( archive.skippedEntries()[ 1 ].startsWith( "pipe: " ) );

   // every other check still applies
   tar = makeTar( "links2.tar", QList< Entry >() << symlink << file( "../evil.txt", "x" ) );
   EXPECT_FALSE( archive.extract( tar, root + "/out2" ) );
   EXPECT_FALSE( QFile::exists( root + "/evil.txt" ) );
}

// A file that cannot be written in full is removed, as one that cannot be
// read in full is.  Past the file size limit every write fails.
TEST_F( TestUSArchive, AFailedWriteRemovesThePartlyWrittenFile )
{
#ifdef Q_OS_LINUX
   QString tar = makeTar( "big.tar", QList< Entry >()
                          << file( "big.bin", QByteArray( 256 * 1024, 'z' ) ) );

   struct rlimit saved;
   ASSERT_EQ( getrlimit( RLIMIT_FSIZE, &saved ), 0 );
   auto oldHandler = signal( SIGXFSZ, SIG_IGN );

   struct rlimit small = saved;
   small.rlim_cur      = 64 * 1024;
   ASSERT_EQ( setrlimit( RLIMIT_FSIZE, &small ), 0 );

   US_Archive archive;
   const bool ok = archive.extract( tar, root + "/out" );

   setrlimit( RLIMIT_FSIZE, &saved );
   signal( SIGXFSZ, oldHandler );

   EXPECT_FALSE( ok );
   EXPECT_FALSE( archive.getError().isEmpty() );
   EXPECT_FALSE( QFile::exists( root + "/out/big.bin" ) );
#else
   GTEST_SKIP() << "needs RLIMIT_FSIZE";
#endif
}

// Extracting over a file that is already there leaves its permissions as
// they were; it used to be left writable by its owner only (mode 0200)
TEST_F( TestUSArchive, AnExistingFileKeepsItsPermissions )
{
   const QString dest = root + "/out";
   const QString path = dest + "/a.txt";
   QDir().mkpath( dest );

   QFile old( path );
   ASSERT_TRUE( old.open( QIODevice::WriteOnly ) );
   old.write( "old" );
   old.close();

   const QFileDevice::Permissions perms = QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ReadGroup | QFileDevice::ReadOther;
   ASSERT_TRUE( QFile::setPermissions( path, perms ) );

   QString    tar = makeTar( "over.tar", QList< Entry >() << file( "a.txt", "new" ) );
   US_Archive archive;
   ASSERT_TRUE( archive.extract( tar, dest ) ) << archive.getError().toStdString();

   EXPECT_TRUE( QFile::permissions( path ).testFlag( QFileDevice::ReadOwner ) );
   EXPECT_TRUE( QFile::permissions( path ).testFlag( QFileDevice::ReadGroup ) );
   EXPECT_TRUE( QFile::permissions( path ).testFlag( QFileDevice::ReadOther ) );

   QFile ff( path );
   ASSERT_TRUE( ff.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( ff.readAll(), QByteArray( "new" ) );
}

// A name with letters outside ASCII is written and read back as itself
TEST_F( TestUSArchive, ANonAsciiNameRoundTrips )
{
   const QString name = QString( "R" ) + QChar( 0x00fc ) + "n_" + QChar( 0x03bb ) + ".auc";
   const QString src  = root + "/src";
   QDir().mkpath( src );

   QFile ff( src + "/" + name );
   ASSERT_TRUE( ff.open( QIODevice::WriteOnly ) );
   ff.write( "data" );
   ff.close();

   QString    tar = root + "/" + QChar( 0x00e9 ) + "t" + QChar( 0x00e9 ) + ".tar.gz";
   US_Archive archive;
   ASSERT_TRUE( archive.compress( QStringList() << src, tar ) ) << archive.getError().toStdString();

   QStringList files;
   ASSERT_TRUE( archive.extract( tar, root + "/out", &files ) ) << archive.getError().toStdString();
   ASSERT_EQ  ( files.size(), 1 );
   EXPECT_TRUE( QFile::exists( root + "/out/src/" + name ) )
      << contentsOf( root + "/out" ).join( ", " ).toStdString();
}
