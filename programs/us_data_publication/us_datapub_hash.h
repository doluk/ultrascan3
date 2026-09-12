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
    installations legitimately differs in its IDs and time stamps.  The
    property fingerprint therefore re-serializes an XML payload in a
    canonical form with the volatile identity attributes dropped, so two
    records that only differ in their IDs, GUIDs and time stamps compare
    equal.
*/
class US_DataPubHash
{
   public:
      //! \brief The SHA-256 digest of a file, as lower-case hexadecimal
      //! \param filename The full path of the file to digest
      //! \returns An empty string when the file cannot be read
      static QString fileHash( const QString& filename );

      //! \brief The SHA-256 digest of a byte array, as hexadecimal
      //! \param data The bytes to digest
      static QString dataHash( const QByteArray& data );

      //! \brief The property fingerprint of a file
      //!
      //! For XML payloads this is the digest of the canonical form
      //! described by \ref canonicalXml; for any other payload it is the
      //! plain file digest.
      //! \param filename The full path of the file to fingerprint
      static QString fingerprint( const QString& filename );

      //! \brief The property fingerprint of XML text
      //! \param xml The XML text to fingerprint
      static QString xmlFingerprint( const QByteArray& xml );

      //! \brief A canonical rendering of XML with volatile keys dropped
      //!
      //! Elements are emitted in document order, attributes in sorted order,
      //! text is whitespace-collapsed and attributes whose name identifies a
      //! record instance (anything ending in \c ID, anything containing
      //! \c guid) or a time stamp (anything containing \c date, \c updated
      //! or \c created) are left out.
      //! \param xml The XML text to render
      //! \returns The canonical text, or an empty array on a parse error
      static QByteArray canonicalXml( const QByteArray& xml );

      //! \brief True when an attribute or element name is left out of the
      //!        canonical rendering
      //! \param name The attribute or element name to test
      static bool    isVolatileKey( const QString& name );

      //! \brief True when the file name looks like an XML payload
      //! \param filename The file name to test
      static bool    isXmlFile( const QString& filename );
};
#endif
