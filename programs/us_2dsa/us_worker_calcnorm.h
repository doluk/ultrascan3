//! \file us_worker_calcnorm.h
#ifndef US_THREAD_WORKCN_H
#define US_THREAD_WORKCN_H

#include <QtCore>

#include "us_extern.h"
#include "us_dataIO.h"
#include "us_simparms.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_solute.h"
#include "us_solve_sim.h"
#include "us_astfem_rsa.h"
#include "us_math2.h"

#ifndef DbgLv
#define DbgLv(a) if(dbg_level>=a)qDebug()
#endif

//! \brief Worker thread task packet
typedef struct work_packet_cn_s
{
   int     thrn;       //!< thread number (1,...)
   int     nthrd;      //!< total threads count
   int     amask;      //!< attributes mask
   int     nsolutes;   //!< number of total solutes for all threads
   int     nwsols;     //!< number of solutes handled by this worker

   int     nss;        //!< grid points per row (X/s direction); 0 = no grid
   int     row0;       //!< first grid row (Y direction) owned by this worker
   int     nrows;      //!< number of grid rows owned by this worker
   int     nsamp;      //!< values per subsampled signature (output)

   double  cff0;       //!< constant f/f0 (or zero)

   QVector< US_Solute >   isolutes;  //!< input solutes
   QVector< US_Solute >   csolutes;  //!< computed solutes

   //! Cosine of the angle to the next-higher-X grid neighbour, one entry
   //!  per entry of csolutes.  -1.0 where the neighbour is not available.
   QVector< double >      coher_x;
   //! Cosine of the angle to the next-higher-Y grid neighbour, one entry
   //!  per entry of csolutes.  -1.0 where the neighbour is not available.
   QVector< double >      coher_y;

   //! Unit-length subsampled signatures of the worker's first grid row,
   //!  laid out as nss consecutive blocks of nsamp values.  Used by the
   //!  caller to close the Y-coherence across worker band boundaries.
   QVector< float >       sig_first;
   //! Unit-length subsampled signatures of the worker's last grid row.
   QVector< float >       sig_last;

   QList< int >           solxs;     //!< solute indexes list

   US_SolveSim::DataSet*  dset;      //!< data set object pointer
} WorkPacketCN;

//! \brief Worker thread to do actual work of 2DSA analysis

//! \class WorkerThreadCalcNorm
//! This class is for each of the individual worker threads that do the
//! actual computational work of calculating norm values.
//!
//! When the caller supplies a grid description (nss, row0, nrows), the
//! worker takes a contiguous band of grid rows and additionally reports
//! the coherence (cosine of the angle) between each column of the A
//! matrix and its immediate grid neighbours.  That coherence, rather than
//! the column norm, is what governs how well NNLS can separate adjacent
//! grid points.  Otherwise the worker falls back to an interleaved share
//! of the solute points and reports norms only.
class WorkerThreadCalcNorm : public QThread
{
   Q_OBJECT

   public:
      WorkerThreadCalcNorm( QObject* parent = 0 );
      ~WorkerThreadCalcNorm();

      enum attr_type { ATTR_S, ATTR_K, ATTR_W, ATTR_V, ATTR_D, ATTR_F };

      //! Bounds on the size of a coherence signature:  at most MAX_SIG_RAD
      //!  radial points by MAX_SIG_SCN scans are retained
      enum { MAX_SIG_RAD = 128, MAX_SIG_SCN = 64 };

   public slots:
      //! \brief Define the work packet for a worker thread
      //! \param workin   Input work definition packet
      void define_work     ( WorkPacketCN& );
      //! \brief Get the results work packet of a completed worker thread
      //! \param workout  Output work definition packet
      void get_result      ( WorkPacketCN& );
      //! \brief Run the worker thread
      void run             ();

   signals:
      void work_progress   ( int );
      void work_complete   ( WorkerThreadCalcNorm* );

   private:

      void calc_norms      ( void );
      void set_comp_attr   ( US_Model::SimulationComponent&,
                             US_Solute&, int );
      //! \brief Build a unit-length subsampled signature of a simulation
      void signature       ( US_DataIO::RawData&, float* );
      //! \brief Dot product of two unit-length signatures
      double coherence     ( const float*, const float* );

      int  thrn;          // thread number (1,...)
      int  nthrd;         // number of total threads
      int  nsolutes;      // number of total input solutes
      int  nwsols;        // number of solutes for this worker thread
      int  amask;         // attribute mask (xyz combination)
      int  attr_x;        // x attribute flag
      int  attr_y;        // y attribute flag
      int  attr_z;        // z attribute flag
      int  dbg_level;     // debug flag

      int  nss;           // grid points per row (0 if no grid given)
      int  row0;          // first grid row owned by this worker
      int  nrows;         // number of grid rows owned by this worker
      int  nsamp;         // values per subsampled signature
      int  sig_nrad;      // radial points per signature
      int  sig_nscn;      // scans per signature
      int  sig_rstr;      // radial stride of a signature
      int  sig_sstr;      // scan stride of a signature

      double  cff0;       // constant f/f0 (or zero)

      US_DataIO::EditedData*  edata;       // experiment data (pointer)
      US_Model                model1;      // output model
      US_SimulationParameters simparms;    // simulation parameters

      US_SolveSim::DataSet*   dset;        // data set object pointer

      QList< int >            solxs;       // list of solute indexes for thread

      QVector< US_Solute >    solutes_i;   // solutes input
      QVector< US_Solute >    solutes_c;   // solutes computed

      QVector< double >       coher_x;     // X-neighbour coherences
      QVector< double >       coher_y;     // Y-neighbour coherences

      QVector< float >        sig_first;   // signatures of first owned row
      QVector< float >        sig_last;    // signatures of last owned row
};

#endif

