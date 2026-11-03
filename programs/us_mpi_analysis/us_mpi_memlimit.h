//! \file us_mpi_memlimit.h
#ifndef US_MPI_MEMLIMIT_H
#define US_MPI_MEMLIMIT_H

#include <QtGlobal>

#include <string>
#include <vector>

//! \brief The memory a job may use on this node, and what it uses now
//!
//! A job's limit is not the node's memory:  the scheduler gives it a share
//! (Slurm's --mem and --mem-per-cpu) and may enforce that with a cgroup.  This
//! class finds the limit and reads the current use.  It makes no MPI or Qt
//! object calls, so a thread may use it next to the rank's MPI calls.  The
//! paths can be replaced for tests.
class US_MPI_MemLimit
{
   public:
      //! \brief Where the system files are
      class Paths
      {
         public:
            Paths() : proc( "/proc" ), cgroup( "/sys/fs/cgroup" ), shm( "/dev/shm" )
            {
            }

            std::string proc;   //!< proc file system
            std::string cgroup; //!< cgroup file system
            std::string shm;    //!< shared memory file system
      };

      //! \brief The cgroup level that limits this process
      class Cgroup
      {
         public:
            qint64      limit = 0;  //!< Limit in bytes (0: no limit found)
            std::string usage_file; //!< File with the bytes charged to the level
            std::string stat_file;  //!< File with the detailed counts of the level
            std::string inactive;   //!< Key in stat_file of reclaimable file pages
      };

      //! \brief Limit from the Slurm environment, in bytes (0: none):
      //!        SLURM_MEM_PER_NODE, or SLURM_MEM_PER_CPU times the CPUs of
      //!        this node (SLURM_CPUS_ON_NODE, else node_ranks)
      static qint64 slurm_limit( int node_ranks );

      //! \brief The tightest limit of this process's cgroup and its parents.
      //!        Of levels with the same limit, the highest is chosen:  all
      //!        ranks of the job are charged to it, not just this rank's task.
      static Cgroup find_cgroup( const Paths& paths = Paths() );

      //! \brief Bytes used at the cgroup level, without reclaimable file
      //!        pages (-1 if unknown)
      static qint64 cgroup_usage( const Cgroup& cgroup );

      //! \brief Bytes free in the shared memory file system (-1 if unknown)
      static qint64 shm_free( const Paths& paths = Paths() );

      //! \brief Read the node's total and available bytes
      static bool   node_memory( qint64& total, qint64& available, const Paths& paths = Paths() );

      //! \brief Bytes of private memory of the given processes
      static qint64 private_bytes( const std::vector<long>& pids, const Paths& paths = Paths() );

      //! \brief Cache capacity allowed for a wanted capacity:  at most half of
      //!        the job's limit (0: none) and most of the free shared memory
      //!        (negative: unknown)
      static qint64 allowed_capacity( qint64 wanted, qint64 job_limit, qint64 shm_bytes );

      //! \brief Capacity that fits the free shared memory (negative: unknown)
      static qint64 fit_shared_memory( qint64 wanted, qint64 shm_bytes );

      //! \brief Pressure from a job's use of its limit
      enum Pressure
      {
         NONE = 0, //!< No action
         SOFT = 1, //!< Stop adding to the cache
         HARD = 2  //!< Empty the cache
      };

      //! \brief Pressure for bytes used of a limit (limit 0: none)
      static Pressure limit_pressure( qint64 used, qint64 limit );

      //! \brief Pressure for the node's available memory
      static Pressure node_pressure( qint64 total, qint64 available );

      //! \brief Pressure for the free bytes in shared memory, given the
      //!        capacity of the cache
      static Pressure shm_pressure( qint64 shm_bytes, qint64 capacity );
};

#endif
