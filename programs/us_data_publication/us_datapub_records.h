//! \file us_datapub_records.h
#ifndef US_DATAPUB_RECORDS_H
#define US_DATAPUB_RECORDS_H

#include <QtCore>

#include "us_datapub_defs.h"
#include "us_rotor.h"
#include "us_hardware.h"

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

      //! \brief The name a record type carries inside its payload file
      //! \param filename The full path of the payload file
      //! \param type     The record type of the payload
      //! \returns An empty string when the name cannot be found
      static QString recordName( const QString& filename,
                                 US_DataPub::EntityType type );

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
