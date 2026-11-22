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
//! - has a name Windows cannot hold: a ':' anywhere, a component that ends
//!   in '.' or ' ', or a component named for a device (CON, PRN, AUX, NUL,
//!   COM1-9, LPT1-9, in any case and with any extension);
//! - has the same name as an earlier entry, or a name that differs from
//!   another (or from a directory above another) only in case, which a
//!   case-insensitive file system would write to one place;
//! - is anything but a regular file or a directory: symbolic links, hard
//!   links, devices, FIFOs and sockets are all refused, unless
//!   \ref setSkipLinks is on, when they are passed over (see below);
//! - declares more than \ref maxEntryBytes bytes, or the entries together
//!   declare more than \ref maxTotalBytes;
//! - would be written through a symbolic link that is already in the
//!   destination, or to a place that is not below the destination once
//!   links are resolved.
//! The sizes are enforced again on what is actually decompressed, since a
//! header can understate them; an extraction stopped that way, or by a
//! failed write, removes the file it was writing and leaves the files it
//! had already written.
//!
//! \section archive_links Skipping links
//! Archives made by older UltraScan versions and by hand can carry symbolic
//! links and other special entries that a program never needed, and those
//! programs used to pass over them.  \ref setSkipLinks( true ) keeps that
//! behaviour for them: such entries are not written, each one is noted in
//! \ref skippedEntries and announced by \ref itemSkipped, and the rest of
//! the archive is extracted.  Every other check still applies.  Refusing
//! the archive is the default.
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
    //!        destination: relative, no ".." component, and nothing
    //!        Windows cannot hold -- no ':', no component ending in '.' or
    //!        ' ', no component named for a device (CON, NUL, COM1, ...).
    //! \param name The entry name, as stored in the archive
    static bool safeEntryName( const QString& name );

    //! \brief Pass over links and special entries instead of refusing
    //!        the archive; see \ref archive_links.  Off by default.
    //! \param skip True to skip them, false to refuse the archive
    void setSkipLinks( bool skip ) { skip_links = skip; }

    //! \brief True when links and special entries are passed over
    bool skipLinks() const { return skip_links; }

    //! \brief The entries the last \ref extract passed over, each as
    //!        "name: why"; empty unless \ref setSkipLinks is on
    QStringList skippedEntries() const { return skipped; }

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

    //! \brief A link or special entry was passed over; see \ref setSkipLinks
    //! \param name The entry name, as stored in the archive
    //! \param why  What kind of entry it is
    void itemSkipped (const QString& name, const QString& why);

    //! \brief A Signal sent when an entry is added to the archive file successfully
    //! \param relative_path
    //! \param absolute_path
    void itemAdded (const QString&, const QString&);

private:
    QString error;
    qint64  max_entry = defaultMaxEntryBytes;
    qint64  max_total = defaultMaxTotalBytes;
    bool    skip_links = false;
    QStringList skipped;

    //! \brief True, with \ref error set, when writing \a target below
    //!        \a root would go through a link already on disk
    bool unsafe_on_disk( const QString& root, const QString& target,
                         const QString& name );

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
