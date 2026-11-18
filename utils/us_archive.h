#ifndef US_ARCHIVE_H
#define US_ARCHIVE_H

#include <QtCore>

#include "us_extern.h"

//! \brief A class to extract and compress archive files
//!        Supported files format: tar, tar.gz, tgz, tar.bz2, tar.xz, .zip
//!
//! Extraction treats the archive as untrusted.  It is read twice: the first
//! pass checks every entry and writes nothing, the second writes.  An
//! archive is refused as a whole, before anything is written, when an entry
//! - has an absolute path, a drive letter, or a ".." component, or a path
//!   that does not stay inside the destination once cleaned;
//! - is anything but a regular file or a directory: symbolic links, hard
//!   links, devices, FIFOs and sockets are all refused;
//! - declares more than \ref maxEntryBytes bytes, or the entries together
//!   declare more than \ref maxTotalBytes.
//! The sizes are enforced again on what is actually decompressed, since a
//! header can understate them; an extraction stopped that way leaves the
//! files it had already written.
class US_UTIL_EXTERN US_Archive : public QObject
{
    Q_OBJECT

public:
    US_Archive() {};

    //! Default limit on the decompressed size of one entry: 8 GiB
    static constexpr qint64 defaultMaxEntryBytes = Q_INT64_C( 8 ) << 30;
    //! Default limit on the decompressed size of a whole archive: 32 GiB
    static constexpr qint64 defaultMaxTotalBytes = Q_INT64_C( 32 ) << 30;

    //! \brief Set the decompression limits used by \ref extract.
    //! \param maxEntry Largest decompressed size of one entry, in bytes
    //! \param maxTotal Largest decompressed size of all entries, in bytes
    void setLimits( qint64 maxEntry, qint64 maxTotal );

    //! \brief The largest decompressed size of one entry, in bytes
    qint64 maxEntryBytes() const { return max_entry; }

    //! \brief The largest decompressed size of a whole archive, in bytes
    qint64 maxTotalBytes() const { return max_total; }

    //! \brief True when an archive entry name may be extracted below a
    //!        destination: relative, no drive letter, no ".." component.
    //! \param name The entry name, as stored in the archive
    static bool safeEntryName( const QString& name );

    //! \brief Method to extract archive file (Supported files: tar, tar.gz, tgz, tar.bz2, tar.xz, .zip).
    //! \param archivePath    Path to the archive file.
    //! \param outputPath     Path to where extracted data will be saved. Default is the path where the archive file is located.
    //! \param outputFileList The optional output list of extracted files.
    //! \return True if file extraction is completed, false otherwise.
    bool extract(const QString&, const QString& = "", QStringList* = nullptr);

    //! \brief Method to compress files and folders into an archive file.
    //! \param sourcePathList List of all files and folders need to be compressed. No need to list the contents of folders.
    //! \param archivePath    If only the filename is given, the output path is taken from the first one. Otherwise, the archive file will be saved in the given path.
    //! After compression is complete, this parameter stores the path to the archive file.
    //! Supported files: tar, tar.gz, tgz, tar.bz2, tar.xz, .zip
    //! \return True if compression is completed, false otherwise; on false
    //!         a partly written archive file is removed.
    bool compress(const QStringList&, QString&);

    //! \brief Method to receive the error string.
    //! \return Error string.
    QString getError();

signals:
    //! \brief A Signal sent when an entry is extracted successfully
    //! \param relative_path
    //! \param absolute_path
    void itemExtracted (const QString&, const QString&);

    //! \brief A Signal sent when an entry is added to the archive file successfully
    //! \param relative_path
    //! \param absolute_path
    void itemAdded (const QString&, const QString&);

private:
    QString error;
    qint64  max_entry = defaultMaxEntryBytes;
    qint64  max_total = defaultMaxTotalBytes;

    //! \brief First pass of \ref extract: check every entry, write nothing.
    bool check_entries( const QString& filename, const QDir& dir );
    QStringList absolute_paths;
    QStringList relative_paths;

    //! \brief List contents of a directory.
    //! \param absolute_path Absolute path to the directory.
    //! \param relative_path Relative path to the directory.
    void list_files(const QString&, const QString&);

};

#endif // US_ARCHIVE_H
