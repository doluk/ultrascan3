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

      /*! \brief Record one more dependency on a record of another type

          A solution depends on every one of its analytes.  The first
          dependency of a type is keyed by the GUID key of that type
          (\c analyteGUID), the further ones by the key with a suffix
          (\c analyteGUID_2, \c analyteGUID_3, ...).  A GUID already listed
          is not added twice.
          \param parentType The record type of the parent
          \param parentGUID The GUID of the parent record
      */
      void    addDepend( US_DataPub::EntityType parentType,
                         const QString& parentGUID );

      //! \brief The GUID of the parent record of a given type
      //! \param parentType The record type of the parent
      //! \returns An empty string when there is no such dependency
      QString depend   ( US_DataPub::EntityType parentType ) const;

      //! \brief The GUIDs of all parent records of a given type
      //! \param parentType The record type of the parents
      QStringList dependsOn( US_DataPub::EntityType parentType ) const;

      //! \brief The record type a \c depends key refers to
      //!
      //! The GUID key of a type, optionally followed by \c _ and a number.
      //! \returns \ref US_DataPub::UnknownType for any other key
      static US_DataPub::EntityType dependType( const QString& key );

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

      //! \brief True when the entity has a type and a GUID
      bool    isValid  ( void ) const;

      /*! \brief The manifest keys of every payload file of the entity

          \c payload, when set, and every attribute whose key ends in
          \c Payload -- the extra files some records carry next to their
          main payload (a time state's field definitions, an RI intensity
          profile, a buffer's component definitions).  Each has its digest
          under US_DataPub::digestKeyOf() of its key.
      */
      QStringList payloadKeys( void ) const;

      //! \brief A short one-line label for lists and trees
      QString label    ( void ) const;
};

/*! \class US_DataPubManifest
    \brief The \c manifest.yaml of a data-publication bundle

    The manifest holds one section per record type, in dependency order:
    every record depends only on records of sections listed before its own.
    It is written as a small, deliberately restricted subset of YAML: a
    block of scalar header keys, followed by one block sequence per record
    type whose items are flat key/value maps plus a single nested
    \c depends map.  The reader accepts exactly that subset, and rejects a
    header key or a section that appears twice, and a record key other than
    \c depends that opens a nested map.

    A section is only written when it was exported: one that was in scope
    but holds nothing is written as \c [], one that was not exported at all
    is left out.
*/
class US_DataPubManifest
{
   public:
      //! \brief Construct an empty manifest
      US_DataPubManifest();

      /*! \brief The manifest format version this build writes

          - 1: every bundle written before format 2.  Some development
            builds (between 27126e8 and fa19fb7) listed a noise record
            carried for a model without noise of its own under that model,
            naming the model it was fitted to in \c fittedToModelGUID; its
            payload names the other model, so those bundles cannot be
            imported, and validate() says so.
          - 2: noise is always listed under the model it was fitted to,
            and every model of the bundle that borrows it is named in
            \c borrowedByModelGUIDs.

          A reader accepts every version up to this one.
      */
      static constexpr int currentVersion = 2;

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

      //! \brief Declare a section exported, even if it stays empty
      //! \param type The record type of the section
      void    declareSection( US_DataPub::EntityType type );

      //! \brief True when the section was exported (it may be empty)
      //! \param type The record type of the section
      bool    isDeclared ( US_DataPub::EntityType type ) const;

      //! \brief Replace the contents of one section, declaring it
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
      //!
      //! A file larger than \ref maxManifestBytes is refused unread.
      //! \param filename The full path of the file to read
      //! \param error    Filled in with a message when reading fails
      //! \returns True when the file could be read and parsed
      bool    read       ( const QString& filename, QString& error );

      /*! \brief Check that the manifest is self-contained and safe

          Every entity needs a GUID, unique within its section; every
          dependency has to be in the manifest, in a section listed before
          the entity's own; and every path the manifest names has to pass
          checkPaths().
          \param error Filled in with a message naming the first gap found
          \returns True when the manifest is self-contained
      */
      bool    validate   ( QString& error ) const;

      /*! \brief Check every path and run ID the manifest names

          Every payload path (\c payload and each \c ...Payload attribute)
          must be a safe bundle-relative path, every \c filename a single
          safe path component, and every run ID -- the name of an
          experiment, the \c runID of the records of a run -- has to be a
          usable run ID.  No two payload paths may name the same file on a
          file system that ignores case.  fromYaml() and validate() both
          apply it, so a manifest naming a path outside the bundle is never
          acted upon.
          \param error Filled in with a message naming the first problem
          \returns True when every path is safe
      */
      bool    checkPaths ( QString& error ) const;

      /*! \brief Check the records against the rules an import applies

          On top of checkPaths(): every record's GUID, and every GUID it
          depends on, is a GUID (US_DataPub::guidIsUsable()); every run type
          passes US_DataPub::runTypeIsUsable(); the raw data, edits and time
          state of a run are named after their run
          (US_DataPub::runFileNameIsUsable()), name the run ID of the
          experiment they depend on, and a time state is
          US_DataPub::timeStateFileName(); and a time state carries its
          \c definitionsPayload.  An export applies it before it packs a
          bundle, so that it never writes one an import refuses.
          \param error Filled in with a message naming the first problem
          \returns True when every record passes
      */
      bool    checkAcceptance( QString& error ) const;

      /*! \brief Record one more dependency of an entity already listed

          A solution that fills channels of several runs depends on every
          one of their experiments, which only become known one run at a
          time.
          \param type       The record type of the entity
          \param guid       The GUID of the entity
          \param parentType The record type of the parent
          \param parentGUID The GUID of the parent
          \returns False when the manifest holds no such entity
      */
      bool    addDependency( US_DataPub::EntityType type, const QString& guid,
                             US_DataPub::EntityType parentType,
                             const QString& parentGUID );

      //! \brief A multi-line "type: count" summary of the manifest
      QString summary    ( void ) const;

      //! \brief The largest manifest file read() accepts, in bytes
      static const int maxManifestBytes = 64 << 20;

   private:
      QMap< int, QList< US_DataPubEntity > > sections;

      //! type -> GUID -> position of its first entity in the section
      QHash< int, QHash< QString, int > >   index;

      static QString escape  ( const QString& );
      static QString unescape( const QString& );
};
#endif
