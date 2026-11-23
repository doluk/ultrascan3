//! \file us_datapub_hash.h
#ifndef US_DATAPUB_HASH_H
#define US_DATAPUB_HASH_H

#include <QtCore>

/*! \class US_DataPubHash
    \brief Digests of bundle payload files

    Two different digests are needed when a bundle is imported.

    The manifest carries the SHA-256 digest of the payload <i>file</i>, which
    makes a bundle verifiable: an import can tell whether a payload was
    truncated or tampered with before it uses it.

    Deciding whether an existing record in the target is "the same" record
    needs something weaker, because a record that was copied between
    installations legitimately differs in its own identity, its numeric IDs
    and its time stamps.  The property fingerprint therefore re-serializes an
    XML payload in a canonical form with exactly those keys dropped.  Every
    other attribute is kept -- including the GUIDs a record uses to name its
    parents, so a caller comparing two records has to put both in terms of
    the same parents first (the importer rewrites its copy of the bundle
    record to name the target's parents before it compares).
*/
class US_DataPubHash
{
   public:
      //! \brief The largest XML payload the fingerprint reads, in bytes
      //!
      //! Record payloads are a few kilobytes and a large Monte Carlo model a
      //! few megabytes.  Anything beyond this is refused rather than read
      //! whole into memory.
      static const qint64 maxXmlSize = 64 * 1024 * 1024;

      //! \brief The SHA-256 digest of a file, as lower-case hexadecimal
      //! \param filename The full path of the file to digest
      //! \returns An empty string when the file cannot be opened or a read
      //!          fails part way, never the digest of part of the file
      static QString fileHash( const QString& filename );

      //! \brief The SHA-256 digest of a byte array, as hexadecimal
      //! \param data The bytes to digest
      static QString dataHash( const QByteArray& data );

      /*! \brief The property fingerprint of a file

          For XML payloads this is the digest of the canonical form described
          by \ref canonicalXml; for any other payload it is the plain file
          digest.
          \param filename   The full path of the file to fingerprint
          \param ownElement The element carrying the record's own identity
          \param ownAttr    The attribute of \a ownElement holding that
                            identity (its GUID); dropped from the first
                            \a ownElement only
          \returns An empty string when the file cannot be read, or is an
                   XML file larger than \ref maxXmlSize
      */
      static QString fingerprint( const QString& filename,
                                  const QString& ownElement = QString(),
                                  const QString& ownAttr    = QString() );

      //! \brief The property fingerprint of XML text
      //! \param xml        The XML text to fingerprint
      //! \param ownElement As for \ref fingerprint
      //! \param ownAttr    As for \ref fingerprint
      static QString xmlFingerprint( const QByteArray& xml,
                                     const QString& ownElement = QString(),
                                     const QString& ownAttr    = QString() );

      /*! \brief A canonical rendering of XML with volatile keys dropped

          Elements are emitted in document order, attributes in sorted order
          and text whitespace-collapsed; attribute values and text have their
          markup characters escaped, so that no value can close its own quote
          or element and two different records cannot share a rendering.
          Left out are the attributes \ref isVolatileOn names (numeric record
          IDs and time stamps, from an explicit list) and the record's own
          identity attribute \a ownAttr on the first \a ownElement.  Nothing
          else is dropped: an attribute such as \c solid or \c liquid, the
          GUID of a parent record, or the ID of a buffer's component, is part
          of what the record means.
          \param xml        The XML text to render
          \param ownElement As for \ref fingerprint
          \param ownAttr    As for \ref fingerprint
          \returns The canonical text, or an empty array on a parse error
      */
      static QByteArray canonicalXml( const QByteArray& xml,
                                      const QString& ownElement = QString(),
                                      const QString& ownAttr    = QString() );

      //! \brief True when an attribute name is left out of every canonical
      //!        rendering: an installation-specific numeric record ID or a
      //!        time stamp, from an explicit list
      //! \param name The attribute name to test
      static bool    isVolatileKey( const QString& name );

      /*! \brief True when an attribute is left out on a given element

          \ref isVolatileKey everywhere except the \c id of a buffer's
          \c component element, which names the component: a component
          element carries nothing else but its concentration, so leaving the
          ID out would make two buffers of different salts the same.  The ID
          is that of an installation's component table, so a caller has to
          put both records in terms of the same table before it compares.
          \param element The name of the element carrying the attribute
          \param name    The attribute name to test
      */
      static bool    isVolatileOn( const QString& element,
                                   const QString& name );

      //! \brief True when the file name looks like an XML payload
      //! \param filename The file name to test
      static bool    isXmlFile( const QString& filename );
};
#endif
