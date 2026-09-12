//! \file us_datapub_manifest.h
#ifndef US_DATAPUB_MANIFEST_H
#define US_DATAPUB_MANIFEST_H

#include <QtCore>

#include "us_datapub_defs.h"

/*! \class US_DataPubEntity
    \brief One record listed in a data-publication manifest

    An entity carries the identity of a record (ID, GUID and name), the
    bundle-relative path of its payload, the SHA-256 digest of that payload
    file and the GUIDs of the records it depends on.

    The ID of a record is only meaningful within the installation it was
    exported from -- records read from a local disk store frequently carry a
    placeholder ID -- so the GUID is what an import matches on.  The ID is
    still written to the manifest because it makes a bundle traceable back
    to the database it came from.
*/
class US_DataPubEntity
{
   public:
      //! \brief Construct an empty entity of unknown type
      US_DataPubEntity();

      //! \brief Construct an empty entity of the given type
      //! \param type The record type of the entity
      explicit US_DataPubEntity( US_DataPub::EntityType type );

      US_DataPub::EntityType type;     //!< The record type
      QString                id;       //!< Numeric record ID as text
      QString                guid;     //!< Global identifier of the record
      QString                name;     //!< runID, filename or description
      QString                filename; //!< Base name of the payload file
      QString                payload;  //!< Bundle-relative payload path
      QString                sha256;   //!< SHA-256 digest of the payload file

      //! Additional descriptive keys, written verbatim to the manifest
      QMap< QString, QString > attrs;

      //! Parent GUIDs, keyed by the GUID key of the parent record type
      QMap< QString, QString > depends;

      //! \brief Record that this entity depends on a record of another type
      //! \param parentType The record type of the parent
      //! \param parentGUID The GUID of the parent record
      void    setDepend( US_DataPub::EntityType parentType,
                         const QString& parentGUID );

      //! \brief The GUID of the parent record of a given type
      //! \param parentType The record type of the parent
      //! \returns An empty string when there is no such dependency
      QString depend   ( US_DataPub::EntityType parentType ) const;

      //! \brief Set a manifest field from its key
      //!
      //! Keys that match the ID, GUID or name key of the entity type fill in
      //! the corresponding member; \c payload, \c sha256 and \c filename fill
      //! in their members; anything else is kept in \ref attrs.
      //! \param key   The manifest key
      //! \param value The value belonging to the key
      void    setField ( const QString& key, const QString& value );

      //! \brief The value of a manifest field, by key
      //! \param key The manifest key to look up
      QString field    ( const QString& key ) const;

      //! \brief All manifest keys of the entity, in output order
      QStringList fieldKeys( void ) const;

      //! \brief True when the entity has a type, a GUID and a payload
      bool    isValid  ( void ) const;

      //! \brief A short one-line label for lists and trees
      QString label    ( void ) const;
};

/*! \class US_DataPubManifest
    \brief The \c manifest.yaml of a data-publication bundle

    The manifest holds one section per record type, in dependency order.
    It is written as a small, deliberately restricted subset of YAML: a
    block of scalar header keys, followed by one block sequence per record
    type whose items are flat key/value maps plus a single nested
    \c depends map.  The reader accepts exactly that subset.
*/
class US_DataPubManifest
{
   public:
      //! \brief Construct an empty manifest
      US_DataPubManifest();

      //! \brief The manifest format version this build writes
      static const int currentVersion = 1;

      int     version;      //!< Manifest format version
      QString created;      //!< UTC creation time, ISO-8601
      QString generator;    //!< Program and version that wrote the bundle
      QString source;       //!< Where the records were read from: db|disk
      QString scope;        //!< The scope key the bundle was exported with
      QString bundleGUID;   //!< A GUID identifying this bundle
      QString comment;      //!< Free-text comment

      //! \brief Drop all header values and all sections
      void    clear      ( void );

      //! \brief Append an entity to the section of its type
      //! \param entity The entity to add
      void    add        ( const US_DataPubEntity& entity );

      //! \brief Replace the contents of one section
      //! \param type     The record type of the section
      //! \param entities The new contents of the section
      void    setSection ( US_DataPub::EntityType type,
                           const QList< US_DataPubEntity >& entities );

      //! \brief The entities of one section
      //! \param type The record type of the section
      QList< US_DataPubEntity > section( US_DataPub::EntityType type ) const;

      //! \brief All entities of the manifest, in dependency order
      QList< US_DataPubEntity > all    ( void ) const;

      //! \brief The record types of the manifest, in dependency order
      static QList< US_DataPub::EntityType > orderedTypes( void );

      //! \brief The number of entities in one section
      //! \param type The record type of the section
      int     count      ( US_DataPub::EntityType type ) const;

      //! \brief The number of entities in the whole manifest
      int     total      ( void ) const;

      //! \brief Look up an entity by type and GUID
      //! \param type The record type to look in
      //! \param guid The GUID to look for
      //! \returns True when the manifest holds such an entity
      bool    contains   ( US_DataPub::EntityType type,
                           const QString& guid ) const;

      //! \brief Look up an entity by type and GUID
      //! \param type  The record type to look in
      //! \param guid  The GUID to look for
      //! \param found Filled in with the entity when it was found
      //! \returns True when the manifest holds such an entity
      bool    entity     ( US_DataPub::EntityType type, const QString& guid,
                           US_DataPubEntity& found ) const;

      //! \brief Serialize the manifest to its YAML representation
      QString toYaml     ( void ) const;

      //! \brief Parse a manifest from its YAML representation
      //! \param text  The YAML text to parse
      //! \param error Filled in with a message when parsing fails
      //! \returns True when the text could be parsed
      bool    fromYaml   ( const QString& text, QString& error );

      //! \brief Write the manifest to a file
      //! \param filename The full path of the file to write
      //! \param error    Filled in with a message when writing fails
      //! \returns True when the file was written
      bool    write      ( const QString& filename, QString& error ) const;

      //! \brief Read the manifest from a file
      //! \param filename The full path of the file to read
      //! \param error    Filled in with a message when reading fails
      //! \returns True when the file could be read and parsed
      bool    read       ( const QString& filename, QString& error );

      //! \brief Check that every dependency of every entity is present
      //! \param error Filled in with a message naming the first gap found
      //! \returns True when the manifest is self-contained
      bool    validate   ( QString& error ) const;

      //! \brief A multi-line "type: count" summary of the manifest
      QString summary    ( void ) const;

   private:
      QMap< int, QList< US_DataPubEntity > > sections;

      static QString escape  ( const QString& );
      static QString unescape( const QString& );
};
#endif
