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
      //! \param error Filled in with a message when the directory fails
      //! \returns True when the staging directory is ready
      bool    createStaging( QString& error );

      //! \brief The staging directory, or an empty string when there is none
      QString stagingPath( void ) const;

      //! \brief The full path of a payload inside the staging directory
      //!
      //! The parent directories of the payload are created as needed.
      //! \param relativePath The bundle-relative path of the payload
      QString stagedPath( const QString& relativePath );

      //! \brief Pack the staging directory into an archive
      //!
      //! The archive is built under a temporary name and moved into place,
      //! so that a name with extra dots in it still produces a valid
      //! \c .tar.gz, and so that a failed pack leaves no partial file.
      //! \param archivePath The full path of the .tar.gz to write
      //! \param error       Filled in with a message when packing fails
      //! \returns True when the archive was written
      bool    pack( const QString& archivePath, QString& error );

      //! \brief Unpack an archive into a private staging directory
      //! \param archivePath The full path of the .tar.gz to read
      //! \param error       Filled in with a message when unpacking fails
      //! \returns True when the archive was unpacked
      bool    unpack( const QString& archivePath, QString& error );

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

      //! \brief Remove the staging directory now
      void    cleanup( void );

   private:
      QString staging;
      QString root;
      bool    keep;

      static bool removeTree( const QString& path );
};
#endif
