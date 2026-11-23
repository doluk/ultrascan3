//! \file us_datapub_records.h
#ifndef US_DATAPUB_RECORDS_H
#define US_DATAPUB_RECORDS_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_rotor.h"
#include "us_hardware.h"
#include "us_buffer.h"

/*! \class US_DataPubRecords
    \brief Payload serialization for the records without a file writer

    Most UltraScan3 records already know how to write themselves to a file
    that another installation can read back, and the bundle simply carries
    that file.  Rotor calibrations and abstract centerpieces are the two
    exceptions: their classes can only write to the fixed local store, or
    not at all.  These helpers write and read exactly the format the
    UltraScan3 local store uses for them, so that a bundle payload is
    interchangeable with the local file of the same record.
*/
class US_DataPubRecords
{
   public:
      //! \brief Write a rotor calibration to a specific file
      //!
      //! Numbers are written with 17 significant digits, so they read back
      //! as the same double; the file only replaces an existing one when
      //! all of it was written.
      //! \param calibration The calibration record to write
      //! \param filename    The full path of the file to write
      //! \returns True when the file was written
      static bool writeCalibration( const US_Rotor::RotorCalibration& calibration,
                                    const QString& filename );

      //! \brief Read a rotor calibration from a specific file
      //! \param filename    The full path of the file to read
      //! \param calibration Filled in with the calibration record
      //! \returns True when the file could be read
      static bool readCalibration ( const QString& filename,
                                    US_Rotor::RotorCalibration& calibration );

      //! \brief Write an abstract centerpiece to a specific file
      //!
      //! As writeCalibration(): full precision, written in one step.
      //! \param centerpiece The centerpiece record to write
      //! \param filename    The full path of the file to write
      //! \returns True when the file was written
      static bool writeCenterpiece( const US_AbstractCenterpiece& centerpiece,
                                    const QString& filename );

      //! \brief Read an abstract centerpiece from a specific file
      //! \param filename    The full path of the file to read
      //! \param centerpiece Filled in with the centerpiece record
      //! \returns True when the file could be read
      static bool readCenterpiece ( const QString& filename,
                                    US_AbstractCenterpiece& centerpiece );

      /*! \brief The GUID a rotor calibration travels under

          Its own GUID, when that is one (US_DataPub::guidIsUsable()); an
          old calibration without a usable one gets a GUID derived from
          what it is -- its rotor's GUID, its ID, both coefficients and its
          date -- so exporting it twice gives the same GUID, and two
          installations only arrive at the same GUID for the same
          calibration, not for two that happen to share an ID.

          Only what a database record and its local copy agree on goes into
          it, so that the same calibration gets the same GUID whether it is
          exported from the database or from disk: the coefficients to six
          significant digits (what the local file keeps of them; both are
          read as floats), the date as ASCII digits, and not the GUID of the
          calibration experiment, which the database does not return.
          \param calibration The calibration record
      */
      static QString calibrationGuid(
            const US_Rotor::RotorCalibration& calibration );

      /*! \brief The GUID an abstract centerpiece travels under

          Its own GUID, when that is one; a table row without a usable one
          gets a GUID derived from its serial number, name, material,
          channels, shape, speed limit and the full geometry, for the same
          reasons as calibrationGuid().
          \param centerpiece The centerpiece record
      */
      static QString centerpieceGuid(
            const US_AbstractCenterpiece& centerpiece );

      /*! \brief Whether an RI intensity profile can be imported

          The \c US_RIProfile XML that US_Experiment::saveRIDisk() writes:
          well-formed, with \c US_RIProfile as its root element and at least
          one \c RI element with a \c value.  An import refuses a bundle
          whose profile is not, so an export does not carry one.
          \param xml The content of the profile file
          \param why Filled in with the reason when it is not usable
      */
      static bool riProfileIsUsable( const QByteArray& xml,
                                     QString* why = nullptr );

      //! \brief The name a record type carries inside its payload file
      //! \param filename The full path of the payload file
      //! \param type     The record type of the payload
      //! \returns An empty string when the name cannot be found
      static QString recordName( const QString& filename,
                                 US_DataPub::EntityType type );

      /*! \brief Write the definitions of buffer components to a file

          A buffer file names its components by ID and concentration only;
          what a component is -- its name, unit and the density and
          viscosity coefficients the buffer's properties are computed from
          -- lives in the installation's component table.  The IDs of one
          installation mean nothing in another, so a bundle carries the
          definitions next to the buffer.  The file has the format of the
          local \c etc/bufferComponents.xml (which
          US_BufferComponent::component() reads), with the coefficients at
          full precision.
          \param components The component definitions to write
          \param filename   The full path of the file to write
          \returns True when the file was written
      */
      static bool writeBufferComponents(
            const QList< US_BufferComponent >& components,
            const QString& filename );

      /*! \brief Read buffer component definitions written by
                 writeBufferComponents()
          \param filename   The full path of the file to read
          \param components Filled in with the definitions, keyed by ID
          \returns True when the file could be read
      */
      static bool readBufferComponents(
            const QString& filename,
            QMap< QString, US_BufferComponent >& components );

      /*! \brief Write buffer component definitions with the buffer's
                 concentrations

          As writeBufferComponents(), and every \c component element also
          carries a \c concentration attribute: the concentration of that
          component in the buffer, at full precision.  The buffer file
          itself rounds it to five decimals (\c 'f',5), which turns 5 uM
          into 0.00001 mM.  The elements are in the order of the buffer's
          components, one for each of them.
          \param components     The component definitions, in buffer order
          \param concentrations The buffer's concentration of each component
          \param filename       The full path of the file to write
          \returns True when the file was written; false also when the two
                   lists differ in length
      */
      static bool writeBufferComponents(
            const QList< US_BufferComponent >& components,
            const QList< double >& concentrations,
            const QString& filename );

      /*! \brief Read the concentrations written by the three-argument
                 writeBufferComponents()

          \param filename       The full path of the file to read
          \param componentIDs   Filled in with the component IDs, in order
          \param concentrations Filled in with their concentrations
          \param why            Filled in when the file cannot be used
          \returns True when the file could be read and every component
                   carries a concentration that is a finite number; a
                   file without any concentration (an older bundle) gives
                   false with empty lists and an empty \a why
      */
      static bool readBufferConcentrations( const QString& filename,
                                            QStringList& componentIDs,
                                            QList< double >& concentrations,
                                            QString* why = nullptr );

      /*! \brief Spectra of a record: the name of each ("extinction",
                 "refraction", "fluorescence") and its wavelength -> value
                 pairs
      */
      typedef QMap< QString, QMap< double, double > > Spectra;

      //! \brief The spectrum names writeSpectra() and readSpectra() accept
      static QStringList spectrumNames( void );

      /*! \brief Write spectra at full precision

          The library writers round them: a buffer file keeps a wavelength
          to 0.1 nm and a value to five significant digits, an analyte file
          a value to six.  Points closer than 0.05 nm then fall onto one.
          A bundle carries the spectra of buffers, analytes and solutions
          in a payload of their own, \c spectraPayload, as

          \verbatim
<!DOCTYPE US_DataPubSpectra>
<US_DataPubSpectra version="1.0">
  <spectrum type="extinction">
    <point wavelength="280" value="1.2345678901234567"/>
  </spectrum>
</US_DataPubSpectra>
          \endverbatim

          with both numbers written so that they read back as the same
          double (\c 'g', 17).  Empty spectra are left out.
          \param spectra  The spectra to write; names other than
                          spectrumNames() are refused
          \param filename The full path of the file to write
          \returns True when the file was written
      */
      static bool writeSpectra( const Spectra& spectra,
                                const QString& filename );

      /*! \brief Read spectra written by writeSpectra()

          The root element has to be \c US_DataPubSpectra, every spectrum
          one of spectrumNames() and listed once, and every point a pair of
          finite numbers, each wavelength once per spectrum.
          \param filename The full path of the file to read
          \param spectra  Filled in with the spectra; emptied on failure
          \param why      Filled in with the reason when it fails
          \returns True when the file could be read
      */
      static bool readSpectra( const QString& filename, Spectra& spectra,
                               QString* why = nullptr );

      /*! \brief The spectra an analyte file of a local store really holds

          US_Analyte's reader adds the extinction at 280 nm it computes from
          a protein's sequence (0 without one) to what the file says, so its
          map cannot tell a measured spectrum from none.  This reads the
          \c extinction, \c refraction and \c fluorescence elements of the
          \c analyte element with the given GUID, and nothing else.
          \param dir     The directory holding the A*.xml analyte files
          \param guid    The analyteGUID to look for
          \param spectra Filled in with the spectra (empty ones left out)
          \returns False when no file there holds that analyte, or the file
                   holding it is not well-formed
      */
      static bool readAnalyteSpectra( const QString& dir, const QString& guid,
                                      Spectra& spectra );

      /*! \brief The date of a record as the digits "yyyy-MM-dd"

          Locale-independent: Qt 5 formats a date with the system locale,
          whose digits need not be ASCII ones.
          \param date The date to write; an invalid one gives ""
      */
      static QString isoDate( const QDate& date );

      /*! \brief Rename a record inside its payload file

          Import renames a record rather than overwriting a record of the
          same name that is already in the target, so the name has to be
          changed in the payload before it is written out.  Everything else
          in the file, its GUID included, is copied through untouched.
          \param filename The full path of the payload file to rewrite
          \param type     The record type of the payload
          \param newName  The name to give the record
          \returns True when the file was rewritten
      */
      static bool setRecordName( const QString& filename,
                                 US_DataPub::EntityType type,
                                 const QString& newName );
};
#endif
