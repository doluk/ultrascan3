//! \file us_mpi_sim_cache.cpp
#include "us_mpi_sim_cache.h"

#include <atomic>
#include <cstring>
#include <new>
#include <thread>

// Shared header of the window
class US_MPI_NodeSimCache::Header
{
   public:
      std::atomic<qint64> data_used; // Bytes of the data area taken
      std::atomic<qint64> entries;   // Simulations stored
      std::atomic<int>    full;      // Flag:  no room left for more
};

// Shared slot of the table
class US_MPI_NodeSimCache::Slot
{
   public:
      std::atomic<int> state;      // SlotState
      int              pad;        // (alignment)
      qint64           offset;     // Data offset of the serialized simulation
      qint64           size;       // Bytes of the serialized simulation
      unsigned char    digest[32]; // SHA-256 of the simulation key
};

// Ranks of different processes use the atomics in the shared window
static_assert( ATOMIC_INT_LOCK_FREE == 2, "lock-free int atomics are required" );
static_assert( ATOMIC_LLONG_LOCK_FREE == 2 && sizeof( qint64 ) == sizeof( long long ), "lock-free 64-bit atomics are required" );

// Round up to a multiple of 8 bytes
static qint64 align8( qint64 bytes )
{
   return ( bytes + 7 ) & ~(qint64) 7;
}

// Create the cache (collective over node_comm)
US_MPI_NodeSimCache::US_MPI_NodeSimCache( MPI_Comm node_comm, qint64 max_bytes, qint64 sim_bytes ) :
    US_SimCache( 0 ),
    comm( node_comm ),
    window( MPI_WIN_NULL ),
    base( nullptr ),
    header( nullptr ),
    table( nullptr ),
    data( nullptr ),
    nslots( 0 ),
    data_size( align8( qMax( (qint64) 0, max_bytes ) ) ),
    rank_hits( 0 ),
    rank_misses( 0 ),
    rank_stored( 0 ),
    rank_rejected( 0 )
{
   int node_rank = 0;
   MPI_Comm_rank( comm, &node_rank );

   // Table at most half full when the data area is
   nslots              = 2 * ( data_size / qMax( (qint64) 1024, sim_bytes ) ) + 1024;
   qint64 header_bytes = align8( sizeof( Header ) );
   qint64 table_bytes  = align8( nslots * (qint64) sizeof( Slot ) );
   qint64 total_bytes  = header_bytes + table_bytes + data_size;

   // The node's first rank allocates all of the window; report a failure
   //  instead of aborting, so that every rank can go on without a cache
   MPI_Comm_set_errhandler( comm, MPI_ERRORS_RETURN );
   char* mybase = nullptr;
   int   rc = MPI_Win_allocate_shared( ( node_rank == 0 ) ? (MPI_Aint) total_bytes : 0, 1, MPI_INFO_NULL, comm, &mybase, &window );
   int   ok = ( rc == MPI_SUCCESS ) ? 1 : 0;
   int   all_ok = 0;
   MPI_Allreduce( &ok, &all_ok, 1, MPI_INT, MPI_MIN, comm );

   if ( all_ok == 0 )
   {
      if ( rc == MPI_SUCCESS )
      {
         MPI_Win_free( &window );
      }
      window = MPI_WIN_NULL;
      MPI_Comm_set_errhandler( comm, MPI_ERRORS_ARE_FATAL );
      return;
   }

   MPI_Comm_set_errhandler( comm, MPI_ERRORS_ARE_FATAL );

   MPI_Aint size = 0;
   int      disp = 1;
   char*    ptr  = nullptr;
   MPI_Win_shared_query( window, 0, &size, &disp, &ptr );

   // Passive access for the life of the cache;  ranks use the memory directly
   MPI_Win_lock_all( MPI_MODE_NOCHECK, window );

   header = reinterpret_cast<Header*>( ptr );
   table  = reinterpret_cast<Slot*>( ptr + header_bytes );
   data   = ptr + header_bytes + table_bytes;

   if ( node_rank == 0 )
   { // Empty header and table
      memset( ptr, 0, header_bytes + table_bytes );
      new ( header ) Header();
      header->data_used.store( 0 );
      header->entries.store( 0 );
      header->full.store( 0 );

      for ( qint64 ii = 0; ii < nslots; ii++ )
      {
         new ( &table[ii].state ) std::atomic<int>( EMPTY );
      }
   }

   MPI_Win_sync( window );
   MPI_Barrier( comm );
   MPI_Win_sync( window );

   base = ptr;
}

// Free the shared window (collective over the node communicator)
US_MPI_NodeSimCache::~US_MPI_NodeSimCache()
{
   if ( window != MPI_WIN_NULL )
   {
      if ( base != nullptr )
      {
         MPI_Win_unlock_all( window );
      }
      MPI_Win_free( &window );
   }
}

// Slot of a key digest:  the slot holding it, or else the first empty slot
//  of its probe sequence (null if the table is full)
US_MPI_NodeSimCache::Slot* US_MPI_NodeSimCache::find( const QByteArray& digest, bool& found )
{
   quint64 start = 0;
   memcpy( &start, digest.constData(), sizeof( start ) );
   found = false;

   for ( qint64 nn = 0; nn < nslots; nn++ )
   {
      Slot* slot  = &table[( start + (quint64) nn ) % (quint64) nslots];
      int   state = slot->state.load( std::memory_order_acquire );

      while ( state == CLAIMED )
      { // Another rank is about to write the digest
         std::this_thread::yield();
         state = slot->state.load( std::memory_order_acquire );
      }

      if ( state == EMPTY )
      {
         return slot;
      }

      if ( memcmp( slot->digest, digest.constData(), sizeof( slot->digest ) ) == 0 )
      {
         found = true;
         return slot;
      }
   }

   return nullptr;
}

// Get a simulation
bool US_MPI_NodeSimCache::fetch( const QByteArray& key, US_DataIO::RawData& simdat )
{
   QByteArray digest = QCryptographicHash::hash( key, QCryptographicHash::Sha256 );
   bool       found  = false;
   Slot*      slot   = find( digest, found );

   if ( found && slot->state.load( std::memory_order_acquire ) == READY && unpack( data + slot->offset, slot->size, simdat ) )
   {
      rank_hits++;
      return true;
   }

   rank_misses++;
   return false;
}

// Store a simulation, unless it is stored (or being stored) already
bool US_MPI_NodeSimCache::store( const QByteArray& key, const US_DataIO::RawData& simdat, bool /*survivor*/ )
{
   QByteArray digest = QCryptographicHash::hash( key, QCryptographicHash::Sha256 );

   while ( true )
   {
      bool  found = false;
      Slot* slot  = find( digest, found );

      if ( found )
      { // Stored, or being stored by another rank
         int state = slot->state.load( std::memory_order_acquire );
         return ( state == READY || state == FILLING );
      }

      if ( slot == nullptr || header->full.load( std::memory_order_relaxed ) != 0 )
      { // No room
         header->full.store( 1, std::memory_order_relaxed );
         rank_rejected++;
         return false;
      }

      int expected = EMPTY;

      if ( !slot->state.compare_exchange_strong( expected, CLAIMED, std::memory_order_acq_rel ) )
      {
         continue; // Another rank took the slot:  look again
      }

      memcpy( slot->digest, digest.constData(), sizeof( slot->digest ) );
      slot->state.store( FILLING, std::memory_order_release );

      QByteArray packed = pack( simdat );
      qint64     size   = packed.size();
      qint64     offset = header->data_used.fetch_add( align8( size ), std::memory_order_relaxed );

      if ( offset + size > data_size )
      { // Data area full:  keep the digest, so the key is not tried again
         header->full.store( 1, std::memory_order_relaxed );
         slot->state.store( DEAD, std::memory_order_release );
         rank_rejected++;
         return false;
      }

      memcpy( data + offset, packed.constData(), size );
      slot->offset = offset;
      slot->size   = size;
      slot->state.store( READY, std::memory_order_release );
      header->entries.fetch_add( 1, std::memory_order_relaxed );
      rank_stored++;
      return true;
   }
}

// Remove all simulations.  Only while no other rank of the node uses the
//  cache (the counts are kept)
void US_MPI_NodeSimCache::clear()
{
   for ( qint64 ii = 0; ii < nslots; ii++ )
   {
      table[ii].state.store( EMPTY, std::memory_order_relaxed );
   }

   header->data_used.store( 0, std::memory_order_relaxed );
   header->entries.store( 0, std::memory_order_relaxed );
   header->full.store( 0, std::memory_order_relaxed );
   std::atomic_thread_fence( std::memory_order_seq_cst );
   MPI_Win_sync( window );
}

// Counts of this rank, with the node's entries and bytes
US_SimCache::Stats US_MPI_NodeSimCache::stats() const
{
   Stats st;
   st.hits      = rank_hits;
   st.misses    = rank_misses;
   st.stored    = rank_stored;
   st.evicted   = 0;
   st.rejected  = rank_rejected;
   st.entries   = header->entries.load( std::memory_order_relaxed );
   st.survivors = 0;
   st.bytes     = qMin( header->data_used.load( std::memory_order_relaxed ), data_size );
   st.max_bytes = data_size;
   return st;
}

// Counts of the whole node (collective over the node communicator)
US_SimCache::Stats US_MPI_NodeSimCache::node_stats() const
{
   Stats  st        = stats();
   qint64 mine[4]   = { st.hits, st.misses, st.stored, st.rejected };
   qint64 summed[4] = { 0, 0, 0, 0 };
   MPI_Allreduce( mine, summed, 4, MPI_LONG_LONG, MPI_SUM, comm );
   st.hits     = summed[0];
   st.misses   = summed[1];
   st.stored   = summed[2];
   st.rejected = summed[3];
   return st;
}
