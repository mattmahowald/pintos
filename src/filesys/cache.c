#include <debug.h>
#include "devices/timer.h"
#include "filesys/cache.h"
#include <string.h>
#include <stdio.h>
#include "threads/thread.h"
#include "threads/malloc.h"

#define BUFFER_SIZE 64
#define READ_AHEAD true

static void read_thread (void *aux UNUSED);
static void flush_thread (void *aux UNUSED);
static void flush (struct cache_entry *entry);
static size_t evict (block_sector_t new_sector);

static struct cache_entry *add_to_cache (block_sector_t sector);
static struct cache_entry *get_cache_entry (block_sector_t sector, bool read_ahead);

void free_hash_entry (struct hash_elem *e, void *aux UNUSED);
unsigned cache_hash (const struct hash_elem *p_, void *aux UNUSED);
bool cache_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                 void *aux UNUSED);
unsigned flush_hash (const struct hash_elem *p_, void *aux UNUSED);
bool flush_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                 void *aux UNUSED);

static struct lock cache_lock;          /* Course lock for entire cache. */
static struct hash buffer_cache;        /* Holds sectors in cache array. */
static struct hash flush_entries;       /* Holds sectors in flush. */
static size_t clock_hand;               /* Position of clock hand. */
static struct lock flusher_lock;        /* Synchs flusher with cleanup. */
static bool done;                       /* Let's flusher know to die. */
static struct cache_entry *entry_array; /* Actual cache array. */
static struct semaphore read_sema;      /* Signals read-ahead to read. */
static struct lock read_ahead_lock;     /* Synchs read-ahead list. */
static struct list read_ahead_list;     /* Read-ahead list. */
static struct condition flush_complete; /* Broadcasts that flush is done. */
static struct semaphore read_dead;

/* List of struct read_blocks lets read-ahead thread bring specified sector
   into the cache. */
struct read_block
{
  block_sector_t to_read;               /* Sector to bring in to cache. */
  struct list_elem elem;                /* List elem. */
}; 

/* Initializes the cache and associated metadata. */
void 
cache_init ()
{
  entry_array = malloc (BUFFER_SIZE * sizeof (struct cache_entry));
  hash_init (&buffer_cache, cache_hash, cache_less, NULL);
  hash_init (&flush_entries, flush_hash, flush_less, NULL);
  cond_init (&flush_complete);
  lock_init (&cache_lock);
  lock_init (&flusher_lock);
  list_init (&read_ahead_list);
  sema_init (&read_sema, 0);
  lock_init (&read_ahead_lock);
  cond_init (&flush_complete);
  sema_init (&read_dead, 0);
  done = false;
  clock_hand = 0;
  thread_create ("flusher", PRI_DEFAULT, flush_thread, NULL);
  thread_create ("reader", PRI_DEFAULT, read_thread, NULL);
}

#define SLEEPER_INTERVAL 30

/* Flusher thread runs in the background, sleeping at regular intervals,
   then flushing every cache entry to disk. */
static void
flush_thread (void *aux UNUSED)
{
  for (;;)
    {
      timer_sleep (SLEEPER_INTERVAL);
      lock_acquire (&flusher_lock);
      if (done)
      {
        lock_release (&flusher_lock);
        return;
      }
      lock_acquire (&cache_lock);
      size_t size = hash_size (&buffer_cache);
      lock_release (&cache_lock);
      size_t i;
      for (i = 0; i < size; i++)
        {
          struct cache_entry *entry = &entry_array [i];
          if (lock_try_acquire (&entry->lock))
            {
              if (entry->flags & DIRTY)
                {
                  entry->flags &= ~DIRTY;
                  block_write (fs_device, entry->sector, entry->data);
                }
              lock_release (&entry->lock);
            }
        }
      lock_release (&flusher_lock);
    }
}

/* Adds SECTOR to the reader thread's list of blocks to read. */
void
cache_add_to_read_ahead (block_sector_t sector)
{
  struct read_block *rb = malloc (sizeof *rb);
  if (!rb)
    PANIC ("malloc failed in read ahead");
  rb->to_read = sector;
  lock_acquire (&read_ahead_lock);
  list_push_back (&read_ahead_list, &rb->elem);
  lock_release (&read_ahead_lock);
  sema_up (&read_sema);
}

/* Reader waits for an entry to be added to the read_ahead_list, then
   reads it in from the cache. */
static void
read_thread (void *aux UNUSED)
{
  for (;;)
    {
      sema_down (&read_sema);
      lock_acquire (&flusher_lock);
      if (done)
      {
        lock_release (&flusher_lock);
        sema_up (&read_dead);
        return;
      }
      lock_release (&flusher_lock);

      lock_acquire (&read_ahead_lock);
      ASSERT (!list_empty (&read_ahead_list));
      struct read_block *rb = list_entry(list_pop_front (&read_ahead_list), 
                                        struct read_block, elem);
      lock_release (&read_ahead_lock);
      // printf("Read thread about to beast up %d\n", rb->to_read);
      get_cache_entry (rb->to_read, READ_AHEAD);
      // printf("Read thread beasted up %d\n", rb->to_read);
      free (rb);
    }
}

/* Flushes a sector to disk, adding the entry to the flush hash to prevent
   users from reading an incorrect file from disk. */
static void
flush (struct cache_entry *entry)
{
  struct flush_entry *flush = malloc (sizeof *flush);
  flush->sector = entry->sector;
  hash_insert (&flush_entries, &flush->elem);
  lock_release (&cache_lock);
  block_write (fs_device, entry->sector, entry->data);
  lock_acquire (&cache_lock);
  hash_delete (&flush_entries, &flush->elem);
  free (flush);
  cond_broadcast (&flush_complete, &cache_lock);
}

/* Second chance clock hand algorithm selects an entry to evict, then 
   flushes the value to disk if necessary. */
static size_t
evict (block_sector_t new_sector)
{
  ASSERT (lock_held_by_current_thread (&cache_lock));

  struct cache_entry *evicted = NULL;
  size_t index = 0;
  while (evicted == NULL)
    {
      struct cache_entry *entry = &entry_array[clock_hand];
      if (lock_try_acquire (&entry->lock))
        {
          if (!(entry->flags & ACCESSED))
            {
              evicted = entry;
              struct hash_entry tmp;
              tmp.sector = evicted->sector;
              struct hash_elem *e = hash_find (&buffer_cache, &tmp.elem);
              hash_delete (&buffer_cache, &tmp.elem);
              free (hash_entry (e, struct hash_entry, elem));

              struct hash_entry *new = malloc (sizeof *new);
              new->array_index = clock_hand;
              new->sector = new_sector;
              new->present = false;
              cond_init (&new->pres);
              hash_insert (&buffer_cache, &new->elem);
              index = clock_hand;

              if (evicted->flags & DIRTY)
                flush (evicted);
              new->present = true;
              cond_broadcast (&new->pres, &cache_lock);
            }
          else
            {
              entry->flags &= ~ACCESSED;
              lock_release (&entry->lock);
            }
        }
      clock_hand = (clock_hand + 1) % BUFFER_SIZE;
    }
  return index;
}

/* Adds to the buffer cache the block at SECTOR*/
static struct cache_entry *
add_to_cache (block_sector_t sector)
{
  struct cache_entry *entry;
  size_t size = hash_size (&buffer_cache);
  if (size != BUFFER_SIZE)
    {
      struct hash_entry *he = malloc (sizeof (*he));
      entry = &entry_array[size];
      he->array_index = size;
      lock_init (&entry->lock);
      lock_acquire (&entry->lock);
      he->sector = sector;
      he->present = true;
      hash_insert (&buffer_cache, &he->elem);
    }
  else
    {
      struct flush_entry tmp;
      tmp.sector = sector;
      struct hash_elem *elem = hash_find (&flush_entries, &tmp.elem);
      while (elem)
        {
          cond_wait (&flush_complete, &cache_lock);
          struct hash_entry tmp_cache;
          tmp_cache.sector = sector;
          struct hash_elem *e = hash_find (&buffer_cache, &tmp_cache.elem);
          if (e)
            {
              entry = &entry_array[hash_entry (elem, struct hash_entry, elem)->array_index];
              lock_acquire (&entry->lock);
              return entry;
            }
          elem = hash_find (&flush_entries, &tmp.elem);
        }
      entry = &entry_array [evict (sector)];
    }

  entry->sector = sector;
  entry->flags = ACCESSED;

  return entry;
}

/* Gets a cache entry from the buffer cache, assuring that the entry has 
   finished flushing and present. */
static struct cache_entry *
get_cache_entry (block_sector_t sector, bool read_ahead)
{
  struct hash_entry tmp;
  struct cache_entry *entry;
  tmp.sector = sector;
  lock_acquire (&cache_lock);
  // if (read_ahead)
  //   {
  //     lock_release (&cache_lock);
  //     return NULL;
  //   }   

  // printf("thread %p about to beast up %d\n", thread_current (), sector);

  for (;;)
    {       
      struct hash_elem *elem = hash_find (&buffer_cache, &tmp.elem);
      if (elem)
        {
          struct hash_entry *he = hash_entry (elem, struct hash_entry, elem);
          if (!he->present)
            {
              cond_wait (&he->pres, &cache_lock);
              continue;
            }
          entry = &entry_array[he->array_index];
          lock_acquire (&entry->lock);
          lock_release (&cache_lock);
        } 
      else
        {
          entry = add_to_cache (sector);    
          lock_release (&cache_lock);
          block_read (fs_device, sector, entry->data);
        }
      if (read_ahead)
        {
          if (!lock_held_by_current_thread (&entry->lock))
            {
              PANIC ("reader");
            }
          lock_release (&entry->lock);
        }
      // printf("thread %p beasted up %d\n", thread_current (), sector);
      return entry;
    }
}

/* Reads from given sector into given buffer. If the sector is not
   already cached, sector is cached. */
void
cache_read (block_sector_t sector, void *buffer, size_t ofs, 
            size_t to_read)
{
  struct cache_entry *entry = get_cache_entry (sector, !READ_AHEAD);
  if (buffer)
    memcpy (buffer, entry->data + ofs, to_read);
  entry->flags |= ACCESSED;
  if (!lock_held_by_current_thread (&entry->lock))
    {
      PANIC ("cache_read");
    }

  lock_release (&entry->lock);
}

/* Writes from the given buffer into given sector on disk. If the 
   sector is not already cached, sector is cached. */
void
cache_write (block_sector_t sector, const void *buffer, size_t ofs, 
            size_t to_write)
{
  struct cache_entry *entry = get_cache_entry (sector, !READ_AHEAD);
  if (buffer)
    memcpy (entry->data + ofs, buffer, to_write);
  entry->flags |= DIRTY | ACCESSED;
  if (!lock_held_by_current_thread (&entry->lock))
    {
      PANIC ("cache_write");
    }
  lock_release (&entry->lock);
}

/* Cleanup function for free hash. */
void 
free_hash_entry (struct hash_elem *e, void *aux UNUSED)
{  
  struct hash_entry *entry = hash_entry (e, struct hash_entry, elem);
  free (entry);
}

/* Frees resources associated with the cache. */
void 
cache_cleanup () 
{
  lock_acquire (&flusher_lock);
  done = true;
  lock_release (&flusher_lock);
  sema_up (&read_sema);
  sema_down (&read_dead);
  size_t i = 0;
  for (i = 0; i < hash_size(&buffer_cache); i++)
    {
      if (entry_array[i].flags & DIRTY)
        block_write (fs_device, entry_array[i].sector, &entry_array[i].data);
    }
  free (entry_array);
  hash_destroy (&buffer_cache, free_hash_entry);
}

/* Hash function for cache entry. */
unsigned 
cache_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct hash_entry *p = hash_entry (p_, struct hash_entry, elem);
  return hash_int (p->sector);
}

/* Comparator function for cache entry. */
bool 
cache_less (const struct hash_elem *a_, const struct hash_elem *b_, 
           void *aux UNUSED)
{
  const struct hash_entry *a = hash_entry (a_, struct hash_entry, elem);
  const struct hash_entry *b = hash_entry (b_, struct hash_entry, elem);

  return a->sector < b->sector;
}


/* Hash function for cache entry. */
unsigned 
flush_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct flush_entry *p = hash_entry (p_, struct flush_entry, elem);
  return hash_int (p->sector);
}

/* Comparator function for cache entry. */
bool 
flush_less (const struct hash_elem *a_, const struct hash_elem *b_, 
           void *aux UNUSED)
{
  const struct flush_entry *a = hash_entry (a_, struct flush_entry, elem);
  const struct flush_entry *b = hash_entry (b_, struct flush_entry, elem);

  return a->sector < b->sector;
}