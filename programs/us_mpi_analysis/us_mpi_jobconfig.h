//! \file us_mpi_jobconfig.h
#ifndef US_MPI_JOBCONFIG_H
#define US_MPI_JOBCONFIG_H

#include <QtCore>

#include "us_simparms.h"
#include "us_solution.h"
#include "us_solve_sim.h"

//! \class US_MPI_JobConfig
//! \brief The job description of an hpcrequest XML file (US_JobSubmit).
//!
//! fromXml() only reads the file: it has no MPI, no output and no global
//! state, and it collects errors instead of aborting.  The interpretation
//! of the values (solute types, defaults, validation) is left to the
//! caller.
class US_MPI_JobConfig
{
   public:
      //! \brief A value that may be unset (std::optional needs C++17; the
      //!        Qt5 build uses C++14)
      template <typename T> class Optional
      {
         public:
            Optional& operator=( const T& value )
            {
               val = value;
               set = true;
               return *this;
            }
            explicit operator bool() const
            {
               return set;
            }
            const T& operator*() const
            {
               return val;
            }
            T value_or( const T& other ) const
            {
               return set ? val : other;
            }

         private:
            bool set = false;
            T    val{};
      };

      //! \brief A GA bucket; limits rounded to 7 significant digits
      class Bucket
      {
         public:
            double x_min = 0.0; //!< Minimum X value
            double x_max = 0.0; //!< Maximum X value
            double y_min = 0.0; //!< Minimum Y value
            double y_max = 0.0; //!< Maximum Y value
      };

      //! \brief The input of one dataset.  Values the file does not give are
      //!        unset; where an element occurs more than once the last wins.
      class DatasetSpec
      {
         public:
            QString                                        auc_file;    //!< Raw AUC file name
            QString                                        edit_file;   //!< Edit file name, as given
            Optional<QString>                              tmst_file;   //!< TimeState file name, as given
            QStringList                                    noise_files; //!< Noise file names

            Optional<double>                               density;              //!< Buffer density
            Optional<double>                               viscosity;            //!< Buffer viscosity
            Optional<double>                               compress;             //!< Buffer compressibility
            Optional<bool>                                 manual;               //!< Manual density/viscosity
            QList<US_Solution::AnalyteInfo>                analytes;             //!< Solution analytes
            bool                                           has_solution = false; //!< A solution element was read

            Optional<int>                                  simpoints;          //!< Simulation points
            Optional<double>                               band_volume;        //!< Band loading volume
            Optional<int>                                  mesh_type;          //!< Radial grid type
            Optional<int>                                  grid_type;          //!< Time grid type
            Optional<double>                               rotor_stretch[2];   //!< Rotor stretch coefficients
            Optional<double>                               centerpiece_bottom; //!< Centerpiece bottom
            Optional<int>                                  cp_sector;          //!< Centerpiece shape index
            Optional<double>                               cp_angle;           //!< Centerpiece angle
            Optional<double>                               cp_pathlen;         //!< Centerpiece path length
            Optional<double>                               cp_width;           //!< Centerpiece width

            QVector<US_SimulationParameters::SpeedProfile> speed_steps;          //!< Speed steps
            QVector<double>                                total_concentrations; //!< total_concentration values

            //! \brief The edit file name to load; for MWL data the wavelength
            //!        range is combined with the wavelength of the AUC file
            QString                                        edit_file_name() const;
      };

      //! \brief Read a job description
      //! \param device  Open device with the XML text
      //! \param source  Name of the source for error messages
      static US_MPI_JobConfig fromXml( QIODevice& device, const QString& source );

      //! \brief Read a job description from a file
      static US_MPI_JobConfig fromFile( const QString& path );

      //! \brief True if the description was read without errors
      bool                    ok() const
      {
         return errors.isEmpty();
      }

      //! \brief The debug level (debug_level parameter, 0 if not given)
      int                          debug_level() const;

      //! \brief The debug timings flag (debug_timings parameter)
      bool                         debug_timings() const;

      //! \brief The comma-separated entries of the debug_text parameter
      QStringList                  debug_text() const;

      //! \brief Datasets as us_mpi_analysis uses them: a timestate file not
      //!        given is taken from the previous dataset, the edit file name
      //!        is completed for MWL data, vbar20 is computed from the
      //!        solution.  The caller owns the returned objects.
      QList<US_SolveSim::DataSet*> make_datasets() const;

      QString                      method;       //!< Analysis method (US_JobSubmit method)
      QString                      cluster;      //!< Cluster short name
      QString                      db_name;      //!< Database name
      QString                      udp_server;   //!< UDP status server address
      quint16                      udp_port = 0; //!< UDP status server port
      QString                      request_id;   //!< Request ID, six digits
      QString                      request_guid; //!< Request GUID

      //! Job parameters as strings by name.  s_min/s_max and ff0_min/ff0_max
      //!  are also stored as x_min/x_max and y_min/y_max; bucket_fixed sets
      //!  xtype, ytype and ztype; the first bucket sets ytype if not yet set;
      //!  a job element sets z_value to "0.0" if not given.
      QMap<QString, QString>       parameters;
      QList<Bucket>                buckets;  //!< GA buckets
      QList<DatasetSpec>           datasets; //!< Datasets in file order
      QStringList                  errors;   //!< Errors found while reading

   private:
      void read_job( QXmlStreamReader& );
      void read_job_parameters( QXmlStreamReader& );
      void read_bucket( const QXmlStreamAttributes&, QString& );
      void read_dataset( QXmlStreamReader&, DatasetSpec& );
      void read_files( QXmlStreamReader&, DatasetSpec& );
      void read_solution( QXmlStreamReader&, DatasetSpec& );
};

#endif
