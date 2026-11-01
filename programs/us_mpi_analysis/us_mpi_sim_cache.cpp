//! \file us_mpi_sim_cache.cpp
#include "us_mpi_sim_cache.h"

#include <atomic>
#include <cstring>
#include <new>
#include <thread>

// Shared state of a generation
class US_MPI_NodeSimCache::Generation
{
   public:
      std::atomic<qint64> data_used; // Bytes of the data area taken
      std::atomic<qint64> entries;   // Simulations stored
      std::atomic<int>    full;      // Flag:  no room left for more
      std::atomic<int>    epoch;     // Odd while the generation is emptied
      std::atomic<int>    users;     // Ranks reading or writing it
      int                 pad;       // (alignment)
};

// Shared header of the window
class US_MPI_NodeSimCache::Header
{
   public:
      std::atomic<int> current;  // Generation simulations are stored in
      std::atomic<int> rotating; // Flag:  a rank empties a generation
      Generation       gen[2];
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

// Results of store_in
enum StoreResult
{
   STORED,  // Stored, or stored already
   SKIPPED, // Known but not stored (no room when it came first)
   NO_ROOM  // Generation full
};

// Round up to a multiple of 8 bytes
static qint64 align8( qint64 bytes )
{
   return ( bytes + 7 ) & ~(qint64) 7;
}

// Create the cache (collective over node_comm)
US_MPI_NodeSimCache::US_MPI_NodeSimCache( MPI_Comm node_comm, qint64 max_bytes, qint64 sim_bytes, Policy policy ) :
    US_SimCache( 0 ),
    comm( node_comm ),
    window( MPI_WIN_NULL ),
    base( nullptr ),
    header( nullptr ),
    table( nullptr ),
    data( nullptr ),
    nslots( 0 ),
    data_size( 0 ),
    keep( policy ),
    ngens( ( policy == RECENT ) ? 2 : 1 ),
    gen_slots( 0 ),
    gen_size( 0 ),
    store_on( true ),
    rank_hits( 0 ),
    rank_misses( 0 ),
    rank_stored( 0 ),
    rank_rejected( 0 ),
    rank_evicted( 0 )
{
   int node_rank = 0;
   MPI_Comm_rank( comm, &node_rank );

   // Table of a generation at most half full when its data area is
   gen_size            = align8( qMax( (qint64) 0, max_bytes ) / ngens );
   gen_slots           = 2 * ( gen_size / qMax( (qint64) 1024, sim_bytes ) ) + 1024;
   data_size           = gen_size * ngens;
   nslots              = gen_slots * ngens;
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
      header->current.store( 0 );
      header->rotating.store( 0 );

      for ( int gg = 0; gg < 2; gg++ )
      {
         header->gen[gg].data_used.store( 0 );
         header->gen[gg].entries.store( 0 );
         header->gen[gg].full.store( 0 );
         header->gen[gg].epoch.store( 0 );
         header->gen[gg].users.store( 0 );
      }

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

// Slot of a key digest in generation gg:  the slot holding it, or else the
//  first empty slot of its probe sequence (null if the table is full)
US_MPI_NodeSimCache::Slot* US_MPI_NodeSimCache::find( int gg, const QByteArray& digest, bool& found )
{
   Slot*   gtable = table + gg * gen_slots;
   quint64 start  = 0;
   memcpy( &start, digest.constData(), sizeof( start ) );
   found = false;

   for ( qint64 nn = 0; nn < gen_slots; nn++ )
   {
      Slot* slot  = &gtable[( start + (quint64) nn ) % (quint64) gen_slots];
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

// Announce this rank's use of generation gg; fails while it is emptied.
//  With the sequentially consistent order of users and epoch, either this
//  rank sees the odd epoch, or the emptying rank sees this rank's use.
bool US_MPI_NodeSimCache::enter( int gg )
{
   Generation& gen = header->gen[gg];
   gen.users.fetch_add( 1, std::memory_order_seq_cst );

   if ( ( gen.epoch.load( std::memory_order_seq_cst ) & 1 ) != 0 )
   {
      gen.users.fetch_sub( 1, std::memory_order_seq_cst );
      return false;
   }

   return true;
}

// End this rank's use of generation gg
void US_MPI_NodeSimCache::leave( int gg )
{
   header->gen[gg].users.fetch_sub( 1, std::memory_order_seq_cst );
}

// Get a simulation from generation gg
bool US_MPI_NodeSimCache::fetch_in( int gg, const QByteArray& digest, US_DataIO::RawData& simdat )
{
   bool  found = false;
   Slot* slot  = find( gg, digest, found );

   return ( found && slot->state.load( std::memory_order_acquire ) == READY &&
            unpack( data + gg * gen_size + slot->offset, slot->size, simdat ) );
}

// Get a simulation
bool US_MPI_NodeSimCache::fetch( const QByteArray& key, US_DataIO::RawData& simdat )
{
   QByteArray digest = QCryptographicHash::hash( key, QCryptographicHash::Sha256 );
   int        cur    = header->current.load( std::memory_order_acquire );

   for ( int nn = 0; nn < ngens; nn++ )
   {
      int gg = ( cur + nn ) % ngens;

      if ( !enter( gg ) )
      {
         continue;
      }

      bool hit = fetch_in( gg, digest, simdat );
      leave( gg );

      if ( hit )
      {
         rank_hits++;

         if ( gg != cur && store_on )
         { // Keep a simulation of the previous generation in use (after
           //  leaving it:  the copy may empty it)
            store( key, simdat, true );
         }

         return true;
      }
   }

   rank_misses++;
   return false;
}

// Store a simulation in generation gg, unless it is stored (or being stored)
//  already
int US_MPI_NodeSimCache::store_in( int gg, const QByteArray& digest, const US_DataIO::RawData& simdat )
{
   Generation& gen = header->gen[gg];

   while ( true )
   {
      bool  found = false;
      Slot* slot  = find( gg, digest, found );

      if ( found )
      { // Stored, or being stored by another rank
         int state = slot->state.load( std::memory_order_acquire );
         return ( state == READY || state == FILLING ) ? STORED : SKIPPED;
      }

      if ( slot == nullptr || gen.full.load( std::memory_order_relaxed ) != 0 )
      { // No room
         gen.full.store( 1, std::memory_order_relaxed );
         return NO_ROOM;
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
      qint64     offset = gen.data_used.fetch_add( align8( size ), std::memory_order_relaxed );

      if ( offset + size > gen_size )
      { // Data area full:  keep the digest, so the key is not tried again
         gen.full.store( 1, std::memory_order_relaxed );
         slot->state.store( DEAD, std::memory_order_release );
         return NO_ROOM;
      }

      memcpy( data + gg * gen_size + offset, packed.constData(), size );
      slot->offset = offset;
      slot->size   = size;
      slot->state.store( READY, std::memory_order_release );
      gen.entries.fetch_add( 1, std::memory_order_relaxed );
      rank_stored++;
      return STORED;
   }
}

// Store a simulation.  US_SolveSim offers each simulation it computes first
//  as a non-survivor, and a surviving one again:  only the first offer counts
//  as rejected when there is no room.
bool US_MPI_NodeSimCache::store( const QByteArray& key, const US_DataIO::RawData& simdat, bool survivor )
{
   if ( !store_on )
   {
      return false;
   }

   QByteArray digest = QCryptographicHash::hash( key, QCryptographicHash::Sha256 );

   for ( int tries = 0; tries < 2; tries++ )
   {
      int gg = header->current.load( std::memory_order_acquire );

      if ( !enter( gg ) )
      {
         break;
      }

      int result = store_in( gg, digest, simdat );
      leave( gg );

      if ( result != NO_ROOM )
      {
         return ( result == STORED );
      }

      if ( keep != RECENT )
      {
         break;
      }

      rotate( gg ); // Make room, then try once more
   }

   if ( !survivor )
   {
      rank_rejected++;
   }

   return false;
}

// Empty the previous generation and make it current, if generation gg is
//  still current and no other rank does so already.  Waits only for ranks
//  within a single fetch or store of the previous generation.
void US_MPI_NodeSimCache::rotate( int gg )
{
   int idle = 0;

   if ( !header->rotating.compare_exchange_strong( idle, 1, std::memory_order_acq_rel ) )
   {
      return; // Another rank rotates
   }

   if ( header->current.load( std::memory_order_acquire ) == gg )
   {
      int         og  = 1 - gg;
      Generation& old = header->gen[og];
      old.epoch.fetch_add( 1, std::memory_order_seq_cst ); // Odd:  keep out

      while ( old.users.load( std::memory_order_seq_cst ) != 0 )
      {
         std::this_thread::yield();
      }

      rank_evicted += old.entries.load( std::memory_order_relaxed );
      empty( og );
      old.epoch.fetch_add( 1, std::memory_order_seq_cst ); // Even:  usable
      header->current.store( og, std::memory_order_release );
   }

   header->rotating.store( 0, std::memory_order_release );
}

// Empty generation gg (no rank may use it)
void US_MPI_NodeSimCache::empty( int gg )
{
   Slot* gtable = table + gg * gen_slots;

   for ( qint64 ii = 0; ii < gen_slots; ii++ )
   {
      gtable[ii].state.store( EMPTY, std::memory_order_relaxed );
   }

   Generation& gen = header->gen[gg];
   gen.data_used.store( 0, std::memory_order_relaxed );
   gen.entries.store( 0, std::memory_order_relaxed );
   gen.full.store( 0, std::memory_order_relaxed );
   std::atomic_thread_fence( std::memory_order_seq_cst );
}

// Remove all simulations.  Only while no other rank of the node uses the
//  cache (the counts are kept)
void US_MPI_NodeSimCache::clear()
{
   for ( int gg = 0; gg < ngens; gg++ )
   {
      empty( gg );
   }

   header->current.store( 0, std::memory_order_relaxed );
   std::atomic_thread_fence( std::memory_order_seq_cst );
   MPI_Win_sync( window );

   forget_dataset_keys();
}

// Counts of this rank, with the node's entries and bytes
US_SimCache::Stats US_MPI_NodeSimCache::stats() const
{
   Stats st;
   st.hits      = rank_hits;
   st.misses    = rank_misses;
   st.stored    = rank_stored;
   st.evicted   = rank_evicted;
   st.rejected  = rank_rejected;
   st.entries   = 0;
   st.survivors = 0;
   st.bytes     = 0;
   st.max_bytes = data_size;

   for ( int gg = 0; gg < ngens; gg++ )
   {
      st.entries += header->gen[gg].entries.load( std::memory_order_relaxed );
      st.bytes += qMin( header->gen[gg].data_used.load( std::memory_order_relaxed ), gen_size );
   }

   return st;
}

// Counts of the whole node (collective over the node communicator)
US_SimCache::Stats US_MPI_NodeSimCache::node_stats() const
{
   Stats  st        = stats();
   qint64 mine[5]   = { st.hits, st.misses, st.stored, st.rejected, st.evicted };
   qint64 summed[5] = { 0, 0, 0, 0, 0 };
   MPI_Allreduce( mine, summed, 5, MPI_LONG_LONG, MPI_SUM, comm );
   st.hits     = summed[0];
   st.misses   = summed[1];
   st.stored   = summed[2];
   st.rejected = summed[3];
   st.evicted  = summed[4];
   return st;
}
