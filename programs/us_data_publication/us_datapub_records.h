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

          Its own GUID; an old calibration without one gets a GUID derived
          from what it is -- its rotor's GUID, its ID, both coefficients at
          full precision, the GUID of its calibration experiment and its
          date -- so exporting it twice gives the same GUID, and two
          installations only arrive at the same GUID for the same
          calibration, not for two that happen to share an ID.
          \param calibration The calibration record
      */
      static QString calibrationGuid(
            const US_Rotor::RotorCalibration& calibration );

      /*! \brief The GUID an abstract centerpiece travels under

          Its own GUID; a table row without one gets a GUID derived from
          its serial number, name, material, channels, shape, speed limit
          and the full geometry, for the same reasons as calibrationGuid().
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
