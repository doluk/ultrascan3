#ifndef US_APROPARMS_H
#define US_APROPARMS_H

#include <QtCore>

#include "us_extern.h"
#include "us_db2.h"

//! A class to define a Run Protocol object for US_ExperimentMain and
//!  related classes
class US_AnaProfParms
{
   public:
      //! \brief Protocol Lab/Rotor controls class
      class AProfParmsEdit
      {
         public:
            AProfParmsEdit();

            //! A test for identical components
            bool operator== ( const AProfParmsEdit& ) const;

            //! A test for unequal components
            inline bool operator!= ( const AProfParmsEdit& p ) const 
            { return ! operator==(p); }

            //! Load controls from XML
            bool fromXml( QXmlStreamReader& );

            //! Save controls to XML
            bool toXml  ( QXmlStreamWriter& );

            QString     laboratory;    //!< Laboratory with rotor
            QString     rotor;         //!< Rotor description
            QString     calibration;   //!< Rotor Calibration description
	    QString     exptype;
	    QString     operatorname; 
	    QString     instrumentname;
	    
            QString     labGUID;       //!< Laboratory GUID
            QString     rotGUID;       //!< Rotor GUID
            QString     calGUID;       //!< Rotor Calibration GUID
            QString     absGUID;       //!< Abstract Rotor GUID

            int         labID;         //!< Laboratory bB Id
            int         rotID;         //!< Rotor DB Id
            int         calID;         //!< Rotor Calibration DB Id
            int         absID;         //!< Abstract Rotor DB Id
	    int         operID;
	    int         instID;
      };

      //! \brief Protocol Speed Steps controls class
      class AProfParms2DSA
      {
         public:
            class SpeedStep
            {
               public:
                  double      speed;         //!< Step rotor speed in rpm
                  double      accel;         //!< Acceleration in rpm/sec
                  double      duration;      //!< Duration in seconds
                  double      delay;         //!< Delay in seconds
		  double      delay_stage;   //!< Delay in seconds
                  double      scanintv;      //!< Scan interval in seconds
		  double      scanintv_min;  //!< Fastest scan interval

                  SpeedStep();

                  bool operator== ( const SpeedStep& ) const;

                  inline bool operator!= ( const SpeedStep& p ) const 
                  { return ! operator==(p); }
            };

//3-------------------------------------------------------------------------->80
            AProfParms2DSA();

            //! A test for identical components
            bool operator== ( const AProfParms2DSA& ) const;

            //! A test for unequal components
            inline bool operator!= ( const AProfParms2DSA& p ) const 
            { return ! operator==(p); }

            //! Load controls from XML
            bool fromXml( QXmlStreamReader& );

            //! Save controls to XML
            bool toXml  ( QXmlStreamWriter& );

            int         nstep;           //!< Number of speed steps
            bool        spin_down;       //!< Flag: spin down at job end
            bool        radial_calib;    //!< Flag: radial calibration

            QVector< SpeedStep > ssteps; //!< The speed steps
      };

      //! \brief Protocol Cells controls class
      class AProfParmsPCSA
      {
         public:
            class CellUse
            {
               public:
                  int         cell;          //!< Cell number

                  QString     centerpiece;   //!< Centerpiece description
                  QString     windows;       //!< Windows (quartz|sapphire)
                  QString     cbalance;      //!< Counterbalance description

                  CellUse();

                  bool operator== ( const CellUse& ) const;

                  inline bool operator!= ( const CellUse& p ) const 
                  { return ! operator==(p); }
            };

            AProfParmsPCSA();

            //! A test for identical components
            bool operator== ( const AProfParmsPCSA& ) const;

            //! A test for unequal components
            inline bool operator!= ( const AProfParmsPCSA& p ) const 
            { return ! operator==(p); }

            //! Load controls from XML
            bool fromXml( QXmlStreamReader& );

            //! Save controls to XML
            bool toXml  ( QXmlStreamWriter& );

            int         ncell;         //!< Number of total cells
            int         nused;         //!< Number of cells used

            QVector< CellUse >  used;  //!< Cells used (cp or cb)
      };


      //! \brief Protocol Upload controls class
      class AProfParmsUpload
      {
         public:
            AProfParmsUpload();

            //! A test for identical components
            bool operator== ( const AProfParmsUpload& ) const;

            //! A test for unequal components
            inline bool operator!= ( const AProfParmsUpload& p ) const 
            { return ! operator==(p); }

            QString     us_xml;        //!< Run protocol XML
            QString     op_json;       //!< Optima JSON
      };

      //! \brief Constructor for the US_AnaProfParms class
      US_AnaProfParms();

      //! A test for protocol equality
      bool operator== ( const US_AnaProfParms& ) const;      

      //! A test for protocol inequality
      inline bool operator!= ( const US_AnaProfParms& p )
         const { return ! operator==(p); }

      //! \brief Read into internal controls from XML
      //! \param xmli   Xml stream to read
      //! \returns      A flag if read was successful.
      bool fromXml( QXmlStreamReader& );

      //! \brief Write internal controls to XML
      //! \param xmlo   Xml stream to write
      //! \returns      A flag if write was successful.
      bool toXml  ( QXmlStreamWriter& );

      //! \brief Function to convert from a time to a day,hour,min.,sec. list
      //! \param sectime  Time in seconds
      //! \param dhms     Returned 4-element list: day, hour, minute, second
      static void timeToList( double&, QList< int >& );

      //! \brief Function to convert from a time to a day,hour,min.,sec. list
      //! \param timeobj  Time object
      //! \param days     Integer days
      //! \param dhms     Returned 4-element list: day, hour, minute, second
      static void timeToList( QTime&, int&, QList< int >& );
//3-------------------------------------------------------------------------->80

      //! \brief Function to convert to a time from a day,hour,min.,sec. list
      //! \param sectime  Returned time in seconds
      //! \param dhms     Input 4-element list: day, hour, minute, second
      static void timeFromList( double&, QList< int >& );

      //! \brief Function to convert to a time from a day,hour,min.,sec. list
      //! \param timeobj  Returned time object
      //! \param days     Returned integer days
      //! \param dhms     Input 4-element list: day, hour, minute, second
      static void timeFromList( QTime&, int&, QList< int >& );

      //! \brief Function to convert from a time to "0d 00:06:30" type string
      //! \param sectime  Time in seconds
      //! \param strtime  Returned time string in "0d 00:06:30" type form
      static void timeToString( double&, QString& );

      //! \brief Function to convert from a time to "0d 00:06:30" type string
      //! \param timeobj  Time object
      //! \param days     Integer days
      //! \param strtime  Returned time string in "0d 00:06:30" type form
      static void timeToString( QTime&, int&, QString& );

      //! \brief Function to convert to a time from a "0d 00:06:30" type string
      //! \param sectime  Returned time in seconds
      //! \param strtime  Input time string in "0d 00:06:30" type form
      static void timeFromString( double&, QString& );

      //! \brief Function to convert to a time from a "0d 00:06:30" type string
      //! \param timeobj  Returned time object
      //! \param days     Returned integer days
      //! \param strtime  Input time string in "0d 00:06:30" type form
      static void timeFromString( QTime&, int&, QString& );

//3-------------------------------------------------------------------------->80
      AProfParmsEdit     apEdit;   //!< Edit controls
      AProfParms2DSA     ap2DSA;   //!< 2DSA controls
      AProfParmsPCSA     apPCSA;   //!< PCSA controls
      AProfParmsUpload   apSubmt;  //!< Upload controls

      QString      investigator;   //!< Investigator name
      QString      runname;        //!< Run ID (name)
      QString      protname;       //!< Protocol name (description)
      QString      pGUID;          //!< Protocol GUID
      QString      project;        //!< Project description
      QString      optimahost;     //!< Optima host (numeric IP address)

      int          projectID; 
      double       temperature;    //!< Run temperature in degrees centigrade
      double       temeq_delay;    //!< Temperature-equilibration delay minutes

   private:
};
#endif
