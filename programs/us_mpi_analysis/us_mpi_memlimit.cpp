//! \file us_mpi_memlimit.cpp
#include "us_mpi_memlimit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef Q_OS_UNIX
#include <sys/statvfs.h>
#endif

// Shares of a limit at which to stop adding to the cache and to empty it.
//  They leave room for what the ranks allocate between two looks at the use.
static const double SOFT_SHARE = 0.75;
static const double HARD_SHARE = 0.88;

// Shares of the node's memory available at which to do the same
static const double NODE_SOFT = 0.05;
static const double NODE_HARD = 0.025;

// Limits at or above this mean "none" (cgroup v1 reports a huge number)
static const qint64 NO_LIMIT = (qint64) 1 << 60;
static const qint64 MB       = 1024 * 1024;

// Read a whole small file (empty if it cannot be read, as on a system without
//  these files, where then no limit or pressure is found)
static std::string  read_text( const std::string& path )
{
   std::string text;
   FILE*       file = fopen( path.c_str(), "r" );

   if ( file == nullptr )
   {
      return text;
   }

   char   buffer[4096];
   size_t count;

   while ( ( count = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
   {
      text.append( buffer, count );
   }

   fclose( file );
   return text;
}

// Number in a file:  -1 if it cannot be read or does not start with a digit
//  (e.g. "max" in cgroup v2)
static qint64 read_number( const std::string& path )
{
   std::string text = read_text( path );

   if ( text.empty() || text[0] < '0' || text[0] > '9' )
   {
      return -1;
   }

   return strtoll( text.c_str(), nullptr, 10 );
}

// Value after a key at the start of a line:  "key value" or "key: value"
static qint64 keyed_value( const std::string& text, const std::string& key )
{
   size_t pos = 0;

   while ( pos < text.size() )
   {
      if ( text.compare( pos, key.size(), key ) == 0 )
      {
         const char* after = text.c_str() + pos + key.size();

         if ( *after == ' ' || *after == ':' || *after == '\t' )
         {
            while ( *after == ' ' || *after == ':' || *after == '\t' )
            {
               after++;
            }

            return strtoll( after, nullptr, 10 );
         }
      }

      pos = text.find( '\n', pos );
      pos = ( pos == std::string::npos ) ? text.size() : pos + 1;
   }

   return -1;
}

// Positive number from the environment (0 if not set or not valid)
static qint64 env_number( const char* name )
{
   const char* text = getenv( name );

   if ( text == nullptr )
   {
      return 0;
   }

   char*  end    = nullptr;
   qint64 number = strtoll( text, &end, 10 );
   return ( end != text && number > 0 ) ? number : 0;
}

// Limit from the Slurm environment.  A value of 0 ("--mem=0") means all the
//  memory of the node, so no limit here.
qint64 US_MPI_MemLimit::slurm_limit( int node_ranks )
{
   const qint64 per_node = env_number( "SLURM_MEM_PER_NODE" );

   if ( per_node > 0 )
   {
      return per_node * MB;
   }

   const qint64 per_cpu = env_number( "SLURM_MEM_PER_CPU" );
   qint64       cpus    = env_number( "SLURM_CPUS_ON_NODE" );
   cpus                 = ( cpus > 0 ) ? cpus : node_ranks;

   return ( per_cpu > 0 ) ? per_cpu * cpus * MB : 0;
}

// The tightest limit of this process's cgroup and its parents
US_MPI_MemLimit::Cgroup US_MPI_MemLimit::find_cgroup( const Paths& paths )
{
   Cgroup      cgroup;
   std::string text = read_text( paths.proc + "/self/cgroup" );
   std::string v1_path;
   std::string v2_path;
   bool        have_v1 = false;
   bool        have_v2 = false;
   size_t      pos     = 0;

   while ( pos < text.size() )
   { // Lines "id:controllers:path"
      size_t end       = text.find( '\n', pos );
      end              = ( end == std::string::npos ) ? text.size() : end;
      std::string line = text.substr( pos, end - pos );
      pos              = end + 1;
      size_t col1      = line.find( ':' );
      size_t col2      = ( col1 == std::string::npos ) ? col1 : line.find( ':', col1 + 1 );

      if ( col2 == std::string::npos )
      {
         continue;
      }

      std::string names = "," + line.substr( col1 + 1, col2 - col1 - 1 ) + ",";

      if ( names == ",," )
      {
         have_v2 = true;
         v2_path = line.substr( col2 + 1 );
      }
      else if ( names.find( ",memory," ) != std::string::npos )
      {
         have_v1 = true;
         v1_path = line.substr( col2 + 1 );
      }
   }

   if ( !have_v1 && !have_v2 )
   {
      return cgroup;
   }

   const std::string base       = have_v1 ? paths.cgroup + "/memory" : paths.cgroup;
   std::string       path       = have_v1 ? v1_path : v2_path;
   const std::string limit_name = have_v1 ? "memory.limit_in_bytes" : "memory.max";
   const std::string usage_name = have_v1 ? "memory.usage_in_bytes" : "memory.current";

   // From this process's level up to the root.  In a container the path of
   //  the host may not exist: such levels are passed over.
   while ( true )
   {
      while ( path.size() > 1 && path.back() == '/' )
      {
         path.pop_back();
      }

      const std::string dir   = base + ( path == "/" ? std::string() : path );
      const qint64      limit = read_number( dir + "/" + limit_name );

      if ( limit > 0 && limit < NO_LIMIT && ( cgroup.limit == 0 || limit <= cgroup.limit ) )
      {
         cgroup.limit      = limit;
         cgroup.usage_file = dir + "/" + usage_name;
         cgroup.stat_file  = dir + "/memory.stat";
         cgroup.inactive   = have_v1 ? "total_inactive_file" : "inactive_file";
      }

      if ( path == "/" || path.empty() )
      {
         break;
      }

      size_t slash = path.rfind( '/' );
      path         = ( slash == 0 || slash == std::string::npos ) ? std::string( "/" ) : path.substr( 0, slash );
   }

   return cgroup;
}

// Bytes used at the cgroup level, without reclaimable file pages
qint64 US_MPI_MemLimit::cgroup_usage( const Cgroup& cgroup )
{
   if ( cgroup.limit <= 0 )
   {
      return -1;
   }

   const qint64 usage = read_number( cgroup.usage_file );

   if ( usage < 0 )
   {
      return -1;
   }

   const qint64 inactive = keyed_value( read_text( cgroup.stat_file ), cgroup.inactive );
   return qMax( (qint64) 0, usage - qMax( (qint64) 0, inactive ) );
}

// Bytes free in the shared memory file system
qint64 US_MPI_MemLimit::shm_free( const Paths& paths )
{
#ifdef Q_OS_UNIX
   struct statvfs info;

   if ( statvfs( paths.shm.c_str(), &info ) != 0 || info.f_blocks == 0 )
   {
      return -1;
   }

   return (qint64) info.f_bavail * (qint64) info.f_frsize;
#else
   Q_UNUSED( paths );
   return -1;
#endif
}

// The node's total and available bytes
bool US_MPI_MemLimit::node_memory( qint64& total, qint64& available, const Paths& paths )
{
   const std::string text = read_text( paths.proc + "/meminfo" );
   total                  = keyed_value( text, "MemTotal" ) * 1024;
   available              = keyed_value( text, "MemAvailable" ) * 1024;
   return ( total > 0 && available >= 0 );
}

// Bytes of private memory of the given processes
qint64 US_MPI_MemLimit::private_bytes( const std::vector<long>& pids, const Paths& paths )
{
   qint64 bytes = 0;

   for ( long pid : pids )
   {
      const qint64 anon = keyed_value( read_text( paths.proc + "/" + std::to_string( pid ) + "/status" ), "RssAnon" );
      bytes += qMax( (qint64) 0, anon ) * 1024;
   }

   return bytes;
}

// Capacity that fits the free shared memory:  touching a page beyond it
//  would kill the process with a bus error
qint64 US_MPI_MemLimit::fit_shared_memory( qint64 wanted, qint64 shm_bytes )
{
   return ( shm_bytes < 0 ) ? wanted : qMin( wanted, shm_bytes / 10 * 9 );
}

// Cache capacity allowed for a wanted capacity
qint64 US_MPI_MemLimit::allowed_capacity( qint64 wanted, qint64 job_limit, qint64 shm_bytes )
{
   qint64 allowed = fit_shared_memory( wanted, shm_bytes );
   return ( job_limit > 0 ) ? qMin( allowed, job_limit / 2 ) : allowed;
}

// Pressure for bytes used of a limit
US_MPI_MemLimit::Pressure US_MPI_MemLimit::limit_pressure( qint64 used, qint64 limit )
{
   if ( limit <= 0 || used < 0 )
   {
      return NONE;
   }

   if ( (double) used > HARD_SHARE * (double) limit )
   {
      return HARD;
   }

   return ( (double) used > SOFT_SHARE * (double) limit ) ? SOFT : NONE;
}

// Pressure for the node's available memory
US_MPI_MemLimit::Pressure US_MPI_MemLimit::node_pressure( qint64 total, qint64 available )
{
   if ( total <= 0 || available < 0 )
   {
      return NONE;
   }

   if ( (double) available < NODE_HARD * (double) total )
   {
      return HARD;
   }

   return ( (double) available < NODE_SOFT * (double) total ) ? SOFT : NONE;
}

// Pressure for the free bytes in shared memory
US_MPI_MemLimit::Pressure US_MPI_MemLimit::shm_pressure( qint64 shm_bytes, qint64 capacity )
{
   if ( shm_bytes < 0 )
   {
      return NONE;
   }

   const qint64 margin = qMax( 64 * MB, capacity / 20 );

   if ( shm_bytes < margin / 2 )
   {
      return HARD;
   }

   return ( shm_bytes < margin ) ? SOFT : NONE;
}
