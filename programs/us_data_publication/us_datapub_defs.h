//! \file us_datapub_defs.h
#ifndef US_DATAPUB_DEFS_H
#define US_DATAPUB_DEFS_H

#include <QtCore>

//! \brief Common enumerations and naming helpers for data-publication bundles
//!
//! A data-publication bundle holds the complete dependency chain of an
//! UltraScan3 data set, starting at the project and ending at the noise
//! records.  The chain is strictly ordered; a bundle always holds a
//! <i>prefix</i> of that order, so that every record in the bundle can be
//! re-created from records that appear earlier in the same bundle.
namespace US_DataPub
{
   //! \brief The kinds of record a bundle can carry
   enum EntityType
   {
      Project = 0,        //!< US_Project record
      Experiment,         //!< US_Experiment record (one run)
      RawData,            //!< One .auc raw-data triple
      RotorCalibration,   //!< US_Rotor::RotorCalibration record
      Centerpiece,        //!< US_AbstractCenterpiece record
      Buffer,             //!< US_Buffer record
      Analyte,            //!< US_Analyte record
      Solution,           //!< US_Solution record
      EditedData,         //!< One edit profile of a raw-data triple
      Model,              //!< US_Model record
      Noise,              //!< US_Noise record
      TimeState,          //!< TimeState data of one run
      UnknownType         //!< Not a known record type
   };

   //! \brief The export scope levels, in dependency order
   //!
   //! Exporting with a given scope means "export everything from
   //! \ref ScopeProject up to and including the given level".  The levels
   //! deliberately follow the order in which the records have to be
   //! re-created on import.
   enum Scope
   {
      ScopeNone = -1,           //!< Nothing selected
      ScopeProject = 0,         //!< project
      ScopeExperiment,          //!< + experiment
      ScopeRawData,             //!< + rawData
      ScopeRotorCalibration,    //!< + rotorCalibration
      ScopeCenterpiece,         //!< + centerpiece
      ScopeBuffers,             //!< + buffers
      ScopeAnalytes,            //!< + analytes
      ScopeSolutions,           //!< + solutions
      ScopeEdits,               //!< + edits
      ScopeModels,              //!< + models
      ScopeNoise                //!< + noise (the full chain)
   };

   //! \brief What to do when a record of the same name already exists
   enum ConflictPolicy
   {
      PolicyReuse = 0,    //!< Point at the existing record when it matches
      PolicyRename,       //!< Always create a new, renamed record
      PolicyFail          //!< Abort the import
   };

   //! \brief Where an import writes its records to
   enum Target
   {
      TargetDb = 0,       //!< The UltraScan3 database
      TargetDisk          //!< A local UltraScan3 data directory
   };

   //! \brief How a single record was dealt with during an import
   enum Resolution
   {
      ResolvedCreated = 0,  //!< A new record was created
      ResolvedReused,       //!< An existing, matching record is used instead
      ResolvedRenamed,      //!< A new record was created under a new name
      ResolvedSkipped,      //!< Nothing was done (dry run)
      ResolvedFailed,       //!< The record could not be imported
      ResolvedSubstituted,  //!< A different record of the target stands in
      ResolvedOmitted       //!< Not imported; the target lacks what it needs
   };

   //! \brief The manifest section name of a record type ("rawData", ...)
   QString typeKey       ( EntityType );

   //! \brief The record type belonging to a manifest section name
   EntityType typeOfKey  ( const QString& );

   //! \brief A human readable, singular name of a record type ("Raw Data")
   QString typeText      ( EntityType );

   //! \brief The manifest key holding the numeric ID ("rawDataID", ...)
   QString idKey         ( EntityType );

   //! \brief The manifest key holding the GUID ("rawDataGUID", ...)
   QString guidKey       ( EntityType );

   //! \brief The record type a GUID key belongs to
   //! \returns \ref UnknownType when the key is not a known GUID key
   EntityType typeOfGuidKey( const QString& );

   //! \brief The manifest key holding the name of a record
   //!
   //! This is the key a human recognizes the record by, and the key the
   //! import conflict rules match on.  It is \c runID for experiments,
   //! \c filename for the file-backed records and \c description for the
   //! records that carry a free-text description.
   QString nameKey       ( EntityType );

   //! \brief The bundle sub-directory holding the payloads of a record type
   QString payloadDir    ( EntityType );

   //! \brief The field definitions file that goes with a time state file
   //!
   //! A time state is a pair of files: the binary \c .tmst and an \c .xml
   //! of the same name holding the field definitions, which US_TimeState
   //! needs to read it.  Only the extension changes, so the name is built
   //! from the last dot rather than by rewriting ".tmst" wherever it
   //! appears -- a directory may be called that too.
   //! \param tmstPath The path of the time state file
   QString timeStateDefs ( const QString& tmstPath );

   //! \brief The record type a given scope level adds
   EntityType scopeType  ( Scope );

   //! \brief The scope level at which a record type starts being exported
   Scope typeScope       ( EntityType );

   //! \brief The CLI name of a scope level ("rawData", "models", ...)
   QString scopeKey      ( Scope );

   //! \brief The scope level belonging to a CLI scope name
   //! \returns \ref ScopeNone when the name is not a scope name
   Scope scopeOfKey      ( const QString& );

   //! \brief All scope names, in dependency order
   QStringList scopeKeys ( void );

   //! \brief The CLI name of a conflict policy ("reuse", "rename", "fail")
   QString policyKey     ( ConflictPolicy );

   //! \brief The conflict policy belonging to a CLI policy name
   //! \param name  The policy name to look up
   //! \param ok    Set to false when the name is not a policy name
   ConflictPolicy policyOfKey( const QString& name, bool* ok = nullptr );

   //! \brief A human readable description of an import resolution
   QString resolutionText( Resolution );
}
//! \brief Safety rules and identity helpers shared by export and import
namespace US_DataPub
{
   /*! \brief Whether a manifest path stays inside the bundle

       A path in a manifest names a file inside the unpacked bundle, and an
       import opens it.  It is only accepted when it is relative, uses
       forward slashes only, and has no empty, ".", ".." or hidden ("."
       first) component, no drive prefix, no colon and no control
       character -- so that no spelling of it can reach outside the bundle
       on any platform.
       \param path The bundle-relative path to check
       \param why  Filled in with the reason when the path is refused
   */
   bool isSafeRelativePath( const QString& path, QString* why = nullptr );

   /*! \brief Whether a manifest file name is a single, safe path component

       A name has to be usable as it is on Linux, macOS and Windows: no
       separator, colon, "." or ".." or hidden name; none of the characters
       < > " | ? *; no control character (C0, DEL or C1) and no invisible
       Unicode formatting character (bidirectional overrides, zero-width
       characters); no trailing dot or space, which Windows drops; not a
       Windows device name (\ref isReservedDeviceName); and at most 255
       bytes in UTF-8.
       \param name The base name to check
       \param why  Filled in with the reason when the name is refused
   */
   bool isSafeFileName    ( const QString& name, QString* why = nullptr );

   /*! \brief Whether a name is a Windows device name

       CON, PRN, AUX, NUL, COM1 to COM9 and LPT1 to LPT9, in any case, are
       devices on Windows whatever extension follows them, so only the part
       before the first dot is compared.
       \param name The file name or run ID to check
   */
   bool isReservedDeviceName( const QString& name );

   /*! \brief Whether a run ID can name a run directory

       The rule US_ConvertIO::runIdIsUsable() applies to a run it reads --
       letters, digits, '_' and '-', 1 to 80 characters -- checked against
       the whole string (no trailing newline), and not a Windows device name.
       \param runID The run ID to check
   */
   bool runIdIsUsable     ( const QString& runID );

   /*! \brief A deterministic, name-based (version 5) UUID

       Used where a record has no GUID of its own -- an old rotor
       calibration, a centerpiece table row, a run's time state -- so that
       exporting the same record twice gives it the same identity, and that
       identity is a valid UUID.
       \param name The text the UUID is derived from
   */
   QString derivedGuid    ( const QString& name );

   /*! \brief Whether a manifest key names a payload file

       \c payload itself, and any attribute whose key ends in \c Payload
       (\c definitionsPayload, \c riProfilePayload, \c componentsPayload)
   */
   bool    isPayloadKey   ( const QString& key );

   /*! \brief The manifest key holding the SHA-256 digest of a payload key

       \c payload goes with \c sha256; \c fooPayload goes with
       \c fooSha256.
   */
   QString digestKeyOf    ( const QString& payloadKey );
}
//! \brief The rules a bundle has to meet to be imported
//!
//! Export and import share these, so that what an export writes is what an
//! import accepts: the export checks its records against them before it
//! packs a bundle, the import refuses a bundle that breaks one.
namespace US_DataPub
{
   /*! \brief Whether a text is a GUID

       Eight, four, four, four and twelve hexadecimal digits, in either
       case, separated by hyphens -- the whole text, without braces and
       without a trailing line end.
       \param guid The text to check
   */
   bool guidIsUsable      ( const QString& guid );

   /*! \brief Whether an XML attribute names a record by its GUID

       An attribute called \c guid or ending in \c GUID
       (\c rawDataGUID, \c solutionGUID, ...), or the \c value of an
       element whose name ends in \c GUID (the way an edit names itself
       and its raw data: \c <editGUID value="..."/>).
       \param element   The name of the element carrying the attribute
       \param attribute The name of the attribute
   */
   bool isGuidAttribute   ( const QString& element,
                            const QString& attribute );

   /*! \brief The first GUID of an XML payload that is not one

       Every attribute isGuidAttribute() takes for a GUID, with a value
       that is not empty, has to pass guidIsUsable().  Empty values are left
       to the record's own checks.
       \param xml The content of the payload
       \returns The offending value, or an empty string when every GUID
                is usable (or the text is not XML at all)
   */
   QString unusableGuidInXml( const QByteArray& xml );

   /*! \brief Whether an XML attribute holds free text rather than a value

       \c name, \c description, \c desc, \c comment, \c label, \c title,
       \c notes, \c runID, \c filename, \c serial and \c unit (in any letter
       case), every GUID (isGuidAttribute()), and the \c value of an element
       of one of those names (the way an experiment names itself:
       \c <name value="..."/>).  A buffer may be called "Inf"; its density
       may not be infinite.
       \param element   The name of the element carrying the attribute
       \param attribute The name of the attribute
   */
   bool isTextAttribute   ( const QString& element,
                            const QString& attribute );

   /*! \brief Whether a value is a number that is not finite

       "nan", "inf" or "infinity" (any letter case, with or without a sign),
       or a decimal number too large for a double ("1e999"): what
       QString::toDouble() turns into a NaN or an infinity, which a record
       reader then takes for the value.
   */
   bool isNonFiniteNumber ( const QString& value );

   /*! \brief The first attribute of an XML payload whose value is a number
              that is not finite

       Every attribute but those isTextAttribute() takes for free text is
       checked with isNonFiniteNumber(): a buffer's density or pH, a
       component's concentration or coefficients, an analyte's vbar20 or
       molecular weight, a solution's common vbar20, storage temperature or
       amounts, a model's variance and meniscus and its components' values,
       the values of a noise vector, ...  A database cannot store such a
       value, and a disk import would write it back as "nan".
       \param xml The content of the payload
       \returns element/\@attribute="value" of the first one, or an empty
                string when every value is finite (or the text is not XML
                at all)
   */
   QString nonFiniteNumberInXml( const QByteArray& xml );

   /*! \brief Whether a run type can be part of a run's file names

       One to eight letters or digits ("RA", "RI", "IP", "FI", "WA", ...),
       checked against the whole text.  A manifest that names none means
       "RA".
       \param runType The run type to check
   */
   bool runTypeIsUsable   ( const QString& runType );

   /*! \brief Whether a file can go into the directory of a run

       Raw data, edits and the time state live in the run's directory, and
       the readers find them there by their run ID: the name has to be a
       safe file name (isSafeFileName()) that starts with the run ID and a
       dot.
       \param fileName The base name of the file
       \param runID    The run the file belongs to
   */
   bool runFileNameIsUsable( const QString& fileName, const QString& runID );

   //! \brief The one name the time state file of a run may have:
   //!        \c <runID>.time_state.tmst
   QString timeStateFileName( const QString& runID );

   /*! \brief The SHA-256 digest of a file, as lower case hexadecimal

       Unlike a digest that stops at the first block it cannot read, a file
       that cannot be opened or read to its end gives no digest at all.
       \param path   The file to digest
       \param digest Filled in with the digest; cleared on failure
       \param why    Filled in with the reason when it fails
       \returns True when the whole file was read
   */
   bool fileSha256        ( const QString& path, QString& digest,
                            QString* why = nullptr );

   /*! \brief Why a master password cannot open the database, if it cannot

       The master password unlocks the database password stored for the
       default database.  A wrong one unlocks nothing, and the server then
       refuses a login without a password ("using password: NO"), which
       says nothing about what is wrong.  This tells it first: the password
       does not match the one this installation set (US_Settings::UltraScanPW,
       when there is one), or it does not unlock the stored database
       password.
       \param masterPW The master password given
       An empty stored database password is unlocked by the right master
       password too, in the current format (an empty ciphertext) as in the
       old AES-128-CBC one (a block of padding, which decrypts to nothing
       as a wrong key does, and is told apart from one).
       \returns The reason, or an empty string when the password unlocks
                the stored database password (or there is none to unlock)
   */
   QString masterPasswordProblem( const QString& masterPW );

   /*! \brief A time stamp as a bundle carries it: UTC, ISO 8601, to the
              second ("2024-03-31T01:30:00Z")

       \param catalogStamp A time stamp as the catalog gives it -- UTC, as
                           "yyyy-MM-dd hh:mm:ss UTC", with a 'T' or a 'Z'
                           now and then, or without the suffix
       \returns The stamp, or an empty string for one that cannot be read
   */
   QString isoUtcStamp    ( const QString& catalogStamp );

   /*! \brief The seconds since the epoch of a UTC time stamp, as
              isoUtcStamp() writes it or as the catalog gives it

       The date and time are taken as UTC whatever the local time zone is:
       none of them is read as a local time, so a time in the local
       spring-forward gap or the repeated hour of the fall-back reads as
       the instant it names.  An explicit offset ("+02:00") is honoured.
       \param stamp   The time stamp
       \param seconds Filled in with the seconds since the epoch
       \returns False for a stamp that cannot be read
   */
   bool utcStampSeconds   ( const QString& stamp, qint64& seconds );
}
#endif
