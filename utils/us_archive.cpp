//! \file us_archive.cpp
#include "us_archive.h"
#include "archive_entry.h"
#include "archive.h"

#include <QRegularExpression>

namespace
{
// The entry name, or an empty string when the archive gives none
QString entry_name( struct archive_entry* entry )
{
    const char* name = archive_entry_pathname_utf8( entry );

    if ( name == nullptr )
        name = archive_entry_pathname( entry );

    return ( name == nullptr ) ? QString() : QString::fromUtf8( name );
}

// Release an archive opened for reading
void close_reader( struct archive* archive )
{
    archive_read_close( archive );
    archive_read_free( archive );
}

// Open an archive for reading, with every format and filter
struct archive* open_reader( const QString& filename, QString& error )
{
    struct archive* archive = archive_read_new();
    archive_read_support_format_all( archive );
    archive_read_support_filter_all( archive );

    if ( archive_read_open_filename( archive, filename.toUtf8().constData(),
                                     10240 ) != ARCHIVE_OK )
    {
        error = QObject::tr( "US_Archive: Error: %1: %2" )
                .arg( archive_error_string( archive ), filename );
        close_reader( archive );
        return nullptr;
    }

    return archive;
}

// True when an entry is something other than a plain file or directory.
// A hard link is stored as a regular file that names another one.
bool unsafe_type( struct archive_entry* entry )
{
    const auto type = archive_entry_filetype( entry );

    return ( type != AE_IFREG  &&  type != AE_IFDIR )  ||
           archive_entry_hardlink( entry ) != nullptr  ||
           archive_entry_symlink ( entry ) != nullptr;
}

// What kind of link or special entry this is, for a note
QString special_kind( struct archive_entry* entry )
{
    if ( archive_entry_hardlink( entry ) != nullptr )  return QObject::tr( "a hard link" );
    if ( archive_entry_symlink ( entry ) != nullptr )  return QObject::tr( "a symbolic link" );

    switch ( archive_entry_filetype( entry ) )
    {
        case AE_IFLNK:  return QObject::tr( "a symbolic link" );
        case AE_IFCHR:
        case AE_IFBLK:  return QObject::tr( "a device" );
        case AE_IFIFO:  return QObject::tr( "a FIFO" );
        case AE_IFSOCK: return QObject::tr( "a socket" );
        default:        return QObject::tr( "a special entry" );
    }
}

// True when a cleaned target path is the destination or below it
bool inside( const QString& root, const QString& target )
{
    const QString clean = QDir::cleanPath( target );

    return clean == root  ||  clean.startsWith( root + "/" );
}

// The components of an entry name, whichever separator, without empty and
// "." components: "./a//b/" is a/b
QStringList name_parts( const QString& name )
{
    QStringList parts = QString( name ).replace( "\\", "/" ).split( "/" );
    parts.removeAll( QString() );
    parts.removeAll( QString( "." ) );
    return parts;
}

// True when Windows keeps a component's name for a device: CON, PRN, AUX,
// NUL, COM1-9 and LPT1-9, in any case, with or without an extension
bool device_name( const QString& part )
{
    static const QRegularExpression rx(
        QRegularExpression::anchoredPattern( "CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]" ),
        QRegularExpression::CaseInsensitiveOption );

    QString base = part.section( ".", 0, 0 );

    while ( base.endsWith( ' ' ) )
        base.chop( 1 );

    return rx.match( base ).hasMatch();
}
}

void US_Archive::setLimits( qint64 maxEntry, qint64 maxTotal )
{
    max_entry = maxEntry;
    max_total = maxTotal;
}

bool US_Archive::safeEntryName( const QString& name )
{
    if ( name.isEmpty() )
        return false;

    // Absolute, on either kind of system, or carrying a drive letter
    if ( name.startsWith( "/" )  ||  name.startsWith( "\\" ) )
        return false;

    // A drive letter, or an NTFS stream, anywhere
    if ( name.contains( ':' ) )
        return false;

    const QStringList parts = name_parts( name );

    if ( parts.isEmpty() )
        return false;

    for ( const QString& part : parts )
    {
        if ( part == ".." )
            return false;

        // Windows drops a trailing dot or space, so "a." and "a" are one file
        if ( part.endsWith( '.' )  ||  part.endsWith( ' ' ) )
            return false;

        if ( device_name( part ) )
            return false;
    }

    return true;
}

bool US_Archive::unsafe_on_disk( const QString& root, const QString& target,
                                 const QString& name )
{
    // Every component below the destination that is already there -- the
    // target itself included -- must not be a link: writing through one
    // would put the file wherever it points
    const QString     rel     = QDir( root ).relativeFilePath( target );
    const QStringList parts   = rel.split( "/", Qt::SkipEmptyParts );
    QString           path    = root;
    QString           deepest = root;

    for ( const QString& part : parts )
    {
        path += "/" + part;
        QFileInfo info( path );

        if ( info.isSymLink() )
        {
            error = QObject::tr( "US_Archive: Error: refusing \"%1\": %2 is a"
                                 " link in the destination" ).arg( name, path );
            return true;
        }

        if ( ! info.exists() )
            break;

        deepest = path;
    }

    // And what is there must resolve to a place below the destination, which
    // also catches links a file system does not report as such
    const QString croot = QFileInfo( root ).canonicalFilePath();
    const QString cdeep = QFileInfo( deepest ).canonicalFilePath();

    if ( ! croot.isEmpty()  &&  ! cdeep.isEmpty()  &&  ! inside( croot, cdeep ) )
    {
        error = QObject::tr( "US_Archive: Error: refusing \"%1\": %2 is not"
                             " inside the destination" ).arg( name, deepest );
        return true;
    }

    return false;
}

bool US_Archive::check_entries( const QString& filename, const QDir& dir )
{
    struct archive* archive = open_reader( filename, error );

    if ( archive == nullptr )
        return false;

    const QString root  = dir.path();
    qint64        total = 0;
    int           result;
    struct archive_entry* entry;

    // Every path the archive writes, directories above its entries included,
    // case-folded: the path as first given, and whether it is a directory
    QHash< QString, QPair< QString, bool > > seen;

    while ( ( result = archive_read_next_header( archive, &entry ) ) == ARCHIVE_OK )
    {
        const QString name = entry_name( entry );

        if ( ! safeEntryName( name )  ||  ! inside( root, root + "/" + name ) )
        {
            error = QObject::tr( "US_Archive: Error: refusing an entry outside the"
                                 " destination, or one Windows cannot hold: \"%1\"" )
                    .arg( name );
            close_reader( archive );
            return false;
        }

        if ( unsafe_type( entry ) )
        {
            if ( skip_links )
            {   // Not written, so there is nothing more to check
                archive_read_data_skip( archive );
                continue;
            }

            error = QObject::tr( "US_Archive: Error: refusing \"%1\": only regular"
                                 " files and directories are extracted" ).arg( name );
            close_reader( archive );
            return false;
        }

        // Two entries that land on one file -- the same name twice, or names
        // a case-insensitive file system does not tell apart -- would have
        // the second overwrite the first, or write into what the first made
        const QStringList parts = name_parts( name );
        const bool        isDir = ( archive_entry_filetype( entry ) == AE_IFDIR );
        QString           path;

        for ( int ii = 0; ii < parts.size(); ii++ )
        {
            path += ( ii == 0 ? QString() : QString( "/" ) ) + parts[ ii ];

            const bool    dirHere = isDir  ||  ii < parts.size() - 1;
            const QString folded  = path.toCaseFolded();

            if ( ! seen.contains( folded ) )
            {
                seen.insert( folded, qMakePair( path, dirHere ) );
                continue;
            }

            const QPair< QString, bool > prior = seen.value( folded );

            if ( prior.first != path )
                error = QObject::tr( "US_Archive: Error: refusing \"%1\": it differs"
                                     " from \"%2\" only in case" )
                        .arg( name, prior.first );

            else if ( ! prior.second  ||  ! dirHere )
                error = QObject::tr( "US_Archive: Error: refusing \"%1\": the"
                                     " archive holds \"%2\" more than once" )
                        .arg( name, path );

            if ( ! error.isEmpty() )
            {
                close_reader( archive );
                return false;
            }
        }

        if ( unsafe_on_disk( root, root + "/" + parts.join( "/" ), name ) )
        {
            close_reader( archive );
            return false;
        }

        if ( archive_entry_filetype( entry ) == AE_IFREG  &&
             archive_entry_size_is_set( entry ) )
        {
            const qint64 size = archive_entry_size( entry );

            if ( size > max_entry )
            {
                error = QObject::tr( "US_Archive: Error: \"%1\" is %2 bytes, more than"
                                     " the limit of %3" )
                        .arg( name ).arg( size ).arg( max_entry );
                close_reader( archive );
                return false;
            }

            total += qMax( size, Q_INT64_C( 0 ) );

            if ( total > max_total )
            {
                error = QObject::tr( "US_Archive: Error: the archive holds more than"
                                     " the limit of %1 bytes" ).arg( max_total );
                close_reader( archive );
                return false;
            }
        }

        archive_read_data_skip( archive );
    }

    if ( result != ARCHIVE_EOF )
    {
        error = QObject::tr( "US_Archive: Error: %1: %2" )
                .arg( archive_error_string( archive ), filename );
        close_reader( archive );
        return false;
    }

    close_reader( archive );
    return true;
}

bool US_Archive::extract(const QString& filename, const QString& path, QStringList* file_list) {

    if ( file_list != nullptr ) {
        file_list->clear();
    }
    error.clear();
    skipped.clear();
    QStringList file_list_in;
    QDir dir;
    QFileInfo fino(filename);
    QString outpath = path.trimmed();
    if (outpath.isEmpty() || outpath.compare(".") == 0) {
        dir.setPath(fino.absolutePath());
    } else if (outpath.startsWith("..")) {
        dir.setPath(fino.absolutePath());
        QString p = dir.absoluteFilePath(outpath);
        dir.setPath(p);
    } else {
        dir.setPath(outpath);
    }
    dir.makeAbsolute();
    dir.setPath( QDir::cleanPath( dir.absolutePath() ) );

    // Everything is checked before anything is written, so a hostile
    // archive leaves no trace in the destination
    if ( ! check_entries( filename, dir ) ) {
        return false;
    }

    if (! dir.mkpath(dir.path())) {
        error = "US_Archive: Error: Could not make the directory: " + dir.path();
        return false;
    }

    struct archive *archive = open_reader( filename, error );
    struct archive_entry *entry;

    if ( archive == nullptr ) {
        return false;
    }

    qint64 total = 0;
    int    result;

    // Loop into the archive file as far as it has files
    while ((result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
        // Get the relative path of entry
        QString entry_path = entry_name( entry );
        QFileInfo target(dir.absoluteFilePath(entry_path));

        // The file is read a second time here, so check again rather than
        // trust that it is the archive the first pass saw
        if ( ! safeEntryName( entry_path )  ||
             ! inside( dir.path(), target.absoluteFilePath() )  ||
             ( unsafe_type( entry )  &&  ! skip_links ) ) {
            error = QObject::tr( "US_Archive: Error: refusing \"%1\"" ).arg( entry_path );
            close_reader( archive );
            return false;
        }

        if ( unsafe_type( entry ) ) {
            const QString why = special_kind( entry );
            skipped << entry_path + ": " + why;
            emit itemSkipped( entry_path, why );
            archive_read_data_skip( archive );
            continue;
        }

        // A link may have appeared in the destination since the first pass
        if ( unsafe_on_disk( dir.path(), target.absoluteFilePath(), entry_path ) ) {
            close_reader( archive );
            return false;
        }

        //If the entry is a directory, make its absolute path
        //If the entry is a file, make its parent's absolute path
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            if (! dir.mkpath(target.absoluteFilePath())) {
                error = "US_Archive: Error: Failed to make path: " + target.absoluteFilePath();
                close_reader( archive );
                return false;
            }
            // emit itemExtracted(entry_path, target.absoluteFilePath());
        } else {
            if (! dir.mkpath(target.absolutePath())) {
                error = "US_Archive: Error: Failed to make path: " + target.absolutePath();
                close_reader( archive );
                return false;
            }

            QFile file(target.absoluteFilePath());
            file.setPermissions(QFileDevice::WriteOwner);
            if (! file.open(QIODevice::WriteOnly)) {
                error = "US_Archive: Error: Failed to open file: " + target.absolutePath();
                close_reader( archive );
                return false;
            }

            const void *buff;
            size_t size;
            la_int64_t offset;
            qint64 written = 0;
            int block;
            //Read data blocks with a buffer to write them in a file
            while ((block = archive_read_data_block(archive, &buff, &size, &offset)) == ARCHIVE_OK) {
                const qint64 end = static_cast<qint64>(offset) + static_cast<qint64>(size);

                // What the header declared was checked already; this is
                // what the data really decompresses to
                total += qMax( end - written, Q_INT64_C( 0 ) );
                if ( end > max_entry  ||  total > max_total ) {
                    error = QObject::tr( "US_Archive: Error: \"%1\" decompresses to more"
                                         " than the size limit" ).arg( entry_path );
                    file.close();
                    file.remove();
                    close_reader( archive );
                    return false;
                }

                // A sparse entry leaves holes between its blocks
                if ( offset != file.pos()  &&  ! file.seek( offset ) ) {
                    error = "US_Archive: Error: Failed to write data blocks: " + target.absolutePath();
                    file.close();
                    file.remove();
                    close_reader( archive );
                    return false;
                }

                const qint64 size_o = file.write(static_cast<const char*>(buff),
                                                 static_cast<qint64>(size));
                if (size_o != static_cast<qint64>(size)) {
                    error = "US_Archive: Error: Failed to write data blocks: " + target.absolutePath();
                    file.close();
                    file.remove();
                    close_reader( archive );
                    return false;
                }
                written = qMax( written, end );
            }

            // Anything but the end of the entry is a damaged archive, and a
            // file cut short there is not one to report as extracted
            if ( block != ARCHIVE_EOF ) {
                error = QObject::tr( "US_Archive: Error: Failed to read \"%1\": %2" )
                        .arg( entry_path, archive_error_string( archive ) );
                file.close();
                file.remove();
                close_reader( archive );
                return false;
            }

            // A sparse entry may end in a hole
            if ( archive_entry_size_is_set( entry )  &&
                 written < archive_entry_size( entry )  &&
                 ! file.resize( archive_entry_size( entry ) ) ) {
                error = "US_Archive: Error: Failed to write data blocks: " + target.absolutePath();
                file.close();
                file.remove();
                close_reader( archive );
                return false;
            }

            // What is still buffered can fail to reach the disk too
            if ( ! file.flush() ) {
                error = "US_Archive: Error: Failed to write data blocks: " + target.absolutePath();
                file.close();
                file.remove();
                close_reader( archive );
                return false;
            }

            file.close();
            emit itemExtracted(entry_path, target.absoluteFilePath());
            file_list_in << target.absoluteFilePath();
        }
        // Skip the current entry data blocks
        archive_read_data_skip(archive);
    }

    if ( result != ARCHIVE_EOF ) {
        error = QObject::tr( "US_Archive: Error: %1: %2" )
                .arg( archive_error_string( archive ), filename );
        close_reader( archive );
        return false;
    }

    close_reader( archive );
    if ( file_list != nullptr) {
        file_list->append(file_list_in);
    }
    return true;
}

bool US_Archive::compress(const QStringList& list, QString& filename) {
    absolute_paths.clear();
    relative_paths.clear();
    if (list.isEmpty()) {
        error = "US_Archive: Error: Empty list";
        return false;
    }

    struct archive *archive;
    archive = archive_write_new();

    error.clear();
    QFileInfo finfo;
    finfo.setFile(filename);
    QString extention = finfo.completeSuffix();
    QDir dir;
    if (finfo.path().compare(".") == 0 || finfo.path().startsWith("..")) {
        finfo.setFile(list.first());
        finfo.makeAbsolute();
        dir.setPath(finfo.absolutePath());
    } else {
        dir.setPath(finfo.absolutePath());
    }
    dir.makeAbsolute();
    filename = dir.absoluteFilePath(filename);

    // Check archive format
    int result;
    bool flag = true;
    if (extention.compare("tar", Qt::CaseInsensitive) == 0) {
        result = archive_write_set_format_pax_restricted(archive);
    }
    else if (extention.compare("tgz", Qt::CaseInsensitive) == 0) {
        result = archive_write_set_format_pax_restricted(archive);
        if (result == ARCHIVE_OK) result = archive_write_add_filter_gzip(archive);
    }
    else if (extention.compare("tar.gz", Qt::CaseInsensitive) == 0) {
        result = archive_write_set_format_pax_restricted(archive);
        if (result == ARCHIVE_OK) result = archive_write_add_filter_gzip(archive);
    }
    else if (extention.compare("tar.xz", Qt::CaseInsensitive) == 0) {
        result = archive_write_set_format_pax_restricted(archive);
        if (result == ARCHIVE_OK) result = archive_write_add_filter_xz(archive);
    }
    else if (extention.compare("tar.bz2", Qt::CaseInsensitive) == 0) {
        result = archive_write_set_format_pax_restricted(archive);
        if (result == ARCHIVE_OK) result = archive_write_add_filter_bzip2(archive);
    }
    else if (extention.compare("zip", Qt::CaseInsensitive) == 0) {
        result = archive_write_set_format_zip(archive);
    } else {
        error = "US_Archive: Error: File format not supported: " + filename;
        flag = false;
    }

    if (flag && result != ARCHIVE_OK) {
        error = QObject::tr("US_Archive: Error: Failed to initialize archive data structure: %1").arg(archive_error_string(archive));
        flag = false;
    }

    if (! flag) {
        archive_write_free(archive);
        return false;
    }

    // List all files and directories
    for (int ii = 0; ii < list.size(); ii++) {
        finfo.setFile(list.at(ii));
        finfo.makeAbsolute();
        QString absolute = finfo.absoluteFilePath();
        QString relative = finfo.fileName();
        if (! finfo.exists()) {
            error = "US_Archive: Error: item not exist: " + absolute;
            archive_write_free(archive);
            return false;
        }
        if (finfo.isFile()) {
            absolute_paths << absolute;
            relative_paths << relative;
        } else if (finfo.isDir()) {
            list_files(absolute, relative);
        }
    }
    if (absolute_paths.size() == 0) {
        error = "US_Archive: Error: Empty file list";
        archive_write_free(archive);
        return false;
    }

    // Create archive file
    result = archive_write_open_filename(archive, filename.toUtf8().constData());
    if (result != ARCHIVE_OK) {
        error = QObject::tr("US_Archive: Error: Failed to create archive file: %1").arg(archive_error_string(archive));
        archive_write_free(archive);
        return false;
    }

    // Loop to add all files to the archive file
    bool written = true;
    for (int ii = 0; ii < absolute_paths.size() && written; ii++) {
        QString absolute = absolute_paths.at(ii);
        QString relative = relative_paths.at(ii);
        struct archive_entry *entry;
        QFile file(absolute);

        if (!file.open(QIODevice::ReadOnly)) {
            error = "US_Archive: Error: Failed to open file: " + absolute;
            written = false;
            break;
        }

        // Write the file header
        entry = archive_entry_new();
        archive_entry_set_pathname(entry, relative.toUtf8().constData());
        archive_entry_set_size(entry, file.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        result = archive_write_header(archive, entry);
        archive_entry_free(entry);

        if (result < ARCHIVE_WARN) {
            error = QObject::tr("US_Archive: Error: Failed to add %1: %2")
                    .arg(absolute, archive_error_string(archive));
            written = false;
            break;
        }

        // Write the file content, a block at a time
        qint64 copied = 0;
        while (! file.atEnd()) {
            QByteArray block = file.read(1024 * 256);
            if (block.isEmpty()) {
                break;
            }
            const la_ssize_t sent = archive_write_data(archive, block.constData(),
                                                       static_cast<size_t>(block.size()));
            if (sent != static_cast<la_ssize_t>(block.size())) {
                error = QObject::tr("US_Archive: Error: Failed to write %1: %2")
                        .arg(absolute, archive_error_string(archive));
                written = false;
                break;
            }
            copied += block.size();
        }

        if (written && copied != file.size()) {
            error = "US_Archive: Error: Failed to read file: " + absolute;
            written = false;
        }

        file.close();

        // emit signal
        if (written) {
            emit itemAdded(relative, absolute);
        }
    }

    // Closing flushes what the compressor still holds, so it can fail too
    result = archive_write_close(archive);
    if (written && result != ARCHIVE_OK) {
        error = QObject::tr("US_Archive: Error: Failed to finish %1: %2")
                .arg(filename, archive_error_string(archive));
        written = false;
    }
    archive_write_free(archive);

    if (! written) {
        QFile::remove(filename);
    }
    return written;
}

void US_Archive::list_files(const QString& abs_path, const QString& base_dir) {

    // Loop folders recursively to list all files
    QDir dir(abs_path);
    QDir::Filters filter = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoDotAndDotDot | QDir::NoSymLinks;
    QFileInfoList list = dir.entryInfoList(filter);
    foreach (const QFileInfo item, list) {
        QString rel_dir = base_dir + "/" + item.fileName();

        if (item.isDir()) {
            list_files(item.absoluteFilePath(), rel_dir);
        } else {
            absolute_paths << item.absoluteFilePath();
            relative_paths << rel_dir;
        }
    }
}

QString US_Archive::getError() {
    return error;
}
