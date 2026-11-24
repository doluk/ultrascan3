//! \file us_datapub_bundle.h
#ifndef US_DATAPUB_BUNDLE_H
#define US_DATAPUB_BUNDLE_H

#include <QtCore>

/*! \class US_DataPubBundle
    \brief The .tar.gz container of a data-publication bundle

    A bundle is a gzip-compressed tar archive whose root holds
    \c manifest.yaml and one directory per record type.  This class deals
    only with the container: staging a directory tree, packing it and
    unpacking it again.  What goes into the tree is decided by
    US_DataPubExporter, and what is made of it by US_DataPubImporter.
*/
class US_DataPubBundle
{
   public:
      //! \brief Construct a bundle with no staging directory yet
      US_DataPubBundle();

      //! \brief Remove the staging directory, unless it was kept
      ~US_DataPubBundle();

      //! \brief The base name of the manifest at the root of a bundle
      static QString manifestName( void );

      //! \brief Create a private staging directory for a new bundle
      //!
      //! With a lock name set (\ref setStagingLock), the lock is taken in
      //! the new directory before this returns.
      //! \param error Filled in with a message when the directory fails
      //! \returns True when the staging directory is ready
      bool    createStaging( QString& error );

      /*! \brief Hold a lock file of this name in every staging directory

          The lock is taken as soon as the directory exists, before anything
          is put into it, and held until \ref cleanup has removed everything
          else: what a process that is still running stages is never found
          without its lock.  A sweep for directories left behind by a killed
          process (\ref sweepStaleStaging) tells them by a lock whose
          process is gone.  The exporter holds \c export.lock, the importer
          \c import.lock.
          \param name The base name of the lock file (it has to end in
                      \c .lock for the sweep to see it); empty for no lock
      */
      void    setStagingLock( const QString& name );

      /*! \brief Remove the staging directories no running process holds

          Every \c bundle_* directory in the work directory of the bundles
          (\c us_datapub in the UltraScan tmp directory) is removed when it
          holds lock files (\c *.lock) and none of them is held by a process
          that is still running, or when it holds no lock and has not
          changed for two days: what an exporter or importer that was
          killed left behind.  The directory of a running process -- one
          whose lock is held, however old -- is never touched.
          \returns The number of directories removed
      */
      static int sweepStaleStaging( void );

      //! \brief True when the staging directory's lock is held
      bool    stagingLocked( void ) const;

      //! \brief The staging directory, or an empty string when there is none
      QString stagingPath( void ) const;

      //! \brief The full path of a payload inside the staging directory
      //!
      //! The parent directories of the payload are created as needed.
      //! \param relativePath The bundle-relative path of the payload
      QString stagedPath( const QString& relativePath );

      /*! \brief A mark of what has been staged so far

          What \ref stagedPath() hands out after it can be taken back with
          \ref unstageSince().
      */
      int     stagedMark( void ) const;

      /*! \brief Take back what was staged since a mark

          Every payload whose path \ref stagedPath() first handed out after
          \a mark is removed, with the directories it leaves empty; a path
          that was handed out before the mark as well is kept.  Only those
          paths are looked at, never the rest of the tree, so taking back
          one record costs what staging it did.
          \param mark  A mark from \ref stagedMark()
          \param error Filled in when a staged file cannot be removed
          \returns True when everything staged since the mark is gone
      */
      bool    unstageSince( int mark, QString& error );

      /*! \brief A scratch directory that goes with the staging directory

          It lies beside the staged tree, not inside it, so whatever is put
          there -- a downloaded time state, a packing work file -- can never
          end up in the bundle.  It is removed with the staging directory.
      */
      QString workPath( void ) const;

      /*! \brief Pack the staging directory into an archive

          The archive is built in the work directory, read back and
          compared with the staged files -- every file, by name, size and
          SHA-256 digest of its content -- and only then written to
          \a archivePath through a QSaveFile, so that an existing file of
          that name is replaced atomically and a failed pack leaves it
          untouched.  The work directory, in the UltraScan tmp directory,
          therefore needs room for the staged tree, the archive and the
          unpacked copy at once: about twice the size of the data plus the
          archive.  A staged hidden file or
          symbolic link is refused rather than silently left out.
          \param archivePath The full path of the .tar.gz to write
          \param error       Filled in with a message when packing fails
          \returns True when the archive was written and verified
      */
      bool    pack( const QString& archivePath, QString& error );

      /*! \brief Unpack an archive into a private staging directory

          The unpacked tree is checked afterwards: a bundle that put
          anything outside the staging directory, or holds a symbolic link,
          is refused.
          \param archivePath The full path of the .tar.gz to read
          \param error       Filled in with a message when unpacking fails
          \returns True when the archive was unpacked
      */
      bool    unpack( const QString& archivePath, QString& error );

      /*! \brief The bundle-relative paths and sizes of the files in a tree
          \param dir   The root of the tree
          \param files Filled in with path -> size, for every regular file
          \param error Filled in when the tree holds a symbolic link, or
                       something that is neither a file nor a directory
          \returns True when the tree could be listed
      */
      static bool listTree( const QString& dir, QMap< QString, qint64 >& files,
                            QString& error );

      //! \brief The directory holding the manifest of an unpacked bundle
      //!
      //! Archives written elsewhere sometimes carry a single top-level
      //! directory, so the manifest is looked for at the root of the
      //! unpacked tree and one level below it.
      //! \returns An empty string when no manifest was found
      QString rootPath( void ) const;

      //! \brief Keep the staging directory when the object goes away
      //! \param keep True to keep the directory
      void    setKeepStaging( bool keep );

      /*! \brief Remove the staging directory now

          The tree goes first and the lock, if one is held, after it: the
          directory is never left without its lock while anything of the
          bundle is still in it.
      */
      void    cleanup( void );

   private:
      QString base;       //!< The private directory holding the two below
      QString staging;    //!< The bundle tree
      QString work;       //!< Scratch space beside it
      QString root;
      bool    keep;
      QString lock_name;  //!< The lock file's base name; empty for none
      QLockFile* lock;    //!< The lock held in base, or null

      //! The paths stagedPath() handed out, in order, and the index each
      //! was first handed out at
      QStringList           staged_rels;
      QHash< QString, int > staged_first;

      bool    verifyEscapes( QString& error ) const;

      static bool removeTree( const QString& path );
};
#endif
