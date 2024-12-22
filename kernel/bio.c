// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

typedef struct buf* Iterator;

typedef struct Bucket {
  struct spinlock lock;
  struct buf headNode;
  Iterator head; // point to headNode
} Bucket;

#define NBUCKET 13
Bucket free_table[NBUCKET];
Bucket hash_table[NBUCKET];
char free_table_lock_names[NBUCKET][14];
char hash_table_lock_names[NBUCKET][16];

/* List Start */
// search with key dev and blockno
// assumption: the list shall locked, return null when failed
Iterator list_search(Iterator head, uint dev, uint blockno) {
  if (head == NULL) panic("search: head is null");
  for (Iterator itr = head->next; itr != head; itr=itr->next) {
    if (itr->dev == dev && itr->blockno == blockno) {
      return itr;
    }
  }
  return NULL;
}

// insert a block to the list, the list shall lock
void list_insert(Iterator head, Iterator newbuf) {
  if (newbuf == NULL) panic("list_insert: insert null element");
  if (newbuf == head) panic("insert head");

  newbuf->next = head->next;
  head->next = newbuf;
  newbuf->prev = head;
  // if (newbuf->next != NULL)
  newbuf->next->prev = newbuf;
}

// pop head from the list, for freelist
Iterator list_pop_head(Iterator head) {
  Iterator ptr = head->next;
  if (ptr == head) return NULL;

  // remove ptr from the list
  head->next = ptr->next;
  // if (ptr->next != NULL) 
  ptr->next->prev = head;

  ptr->next = ptr->prev = NULL;
  return ptr;
}


// search && remove
Iterator list_erase(Iterator itr) {
  if (itr == NULL) panic("list_erase: erase null element");
  if (itr->next == itr || itr->prev == itr) panic("remove head");
  Iterator prev = itr->prev;
  Iterator next = itr->next;

  if (prev == NULL) panic("list_erase: prev of itr shall not be null");
  if (prev->next != itr) panic("list_erase: not a doubly linked list");

  prev->next = next;
  next->prev = prev;
  itr->prev = NULL;
  itr->next = NULL;
  return itr;
}

// pop tail from the list
Iterator list_pop_back(Iterator head) {
  Iterator ptr = head->prev;
  if (ptr == head) return NULL;

  list_erase(ptr);
  return ptr;
}
/* List End */


/* Free Hash Table Start*/
void init_free_table() {
  // init lock & headptr
  // char lock_name[14];
  for (int i=0; i < NBUCKET; ++i) {
    snprintf(free_table_lock_names[i], sizeof(free_table_lock_names[i]), "bcache.free%d", i);
    initlock(&(free_table[i].lock), free_table_lock_names[i]);
    free_table[i].head = &(free_table[i].headNode);
    free_table[i].head->next = free_table[i].head;
  }

  // round robbin insert freepages
  int j = 0; // pointer to free_table
  for (int i=0; i < NBUF; ++i) {
    list_insert(free_table[j].head, &(bcache.buf[i]));
    j = (j + 1) % NBUCKET;
  }
}

// alloc from free table
Iterator block_alloc(uint slot) {
  if (slot < 0 || slot >= NBUCKET) panic("block_alloc");

  Iterator block = NULL;
  acquire(&(free_table[slot].lock));
  block = list_pop_back(free_table[slot].head);
  release(&(free_table[slot].lock));

  // steal
  for (int i=0; i < NBUCKET && block == NULL; ++i) {
    acquire(&(free_table[i].lock));
    block = list_pop_back(free_table[i].head);
    release(&(free_table[i].lock));
  }

  if (block == NULL) panic("block_alloc: no buffers");
  return block;
}

void block_free(uint slot, Iterator itr) {
  if (slot < 0 || slot >= NBUCKET) panic("block_alloc");
  if (itr->refcnt != 0) panic("block_free: ref_count != 0");

  acquire(&(free_table[slot].lock));
  list_insert(free_table[slot].head, itr); 
  release(&(free_table[slot].lock));
}

/* Free Hash Table End*/

/* Hash Table Start*/
uint hash(uint dev, uint blockno) {
  return blockno % NBUCKET;
}

void init_hash_table() {
  // init lock & headptr
  for (int i=0; i < NBUCKET; ++i) {
    snprintf(hash_table_lock_names[i], 
      sizeof(hash_table_lock_names[i]), "bcache.bucket%d", i);
    initlock(&(hash_table[i].lock), hash_table_lock_names[i]);
    hash_table[i].head = &(hash_table[i].headNode);
    hash_table[i].head->next = hash_table[i].head;
  }
}

// get a block from the hash table
Iterator hash_get(uint dev, uint blockno) {
  uint slot = hash(dev, blockno);

  // search block in the slot
  Iterator block = NULL;
  acquire(&(hash_table[slot].lock));
  block = list_search(hash_table[slot].head, dev, blockno);
  // release(&(hash_table[slot].lock));

  // In the hashlist we cannot found the page, search the freelist
  if (block == NULL) {
    acquire(&(free_table[slot].lock));
    block = list_search(free_table[slot].head, dev, blockno);

    if (block != NULL) {
      list_erase(block);
      list_insert(hash_table[slot].head, block);
    }
    release(&(free_table[slot].lock));
  }

  // not found evick a page
  if (block == NULL) {
    block = block_alloc(slot); // lock: hash_table then free_table
    list_insert(hash_table[slot].head, block);
  }

  // update reference count
  if (block->refcnt > 0) {
    block->refcnt += 1;
  }
  else {
      // first process to get the block
      if (block->dev != dev || block->blockno != blockno) {
        block->dev = dev;
        block->blockno = blockno;
        block->valid = 0;
      }
      block->refcnt = 1;
  }

  release(&(hash_table[slot].lock));
  return block;
}

// return a block from hashtable to free list
void hash_release(Iterator block) {
  if (block == NULL) panic("hash_relase");

  uint dev = block->dev, blockno = block->blockno;
  uint slot = hash(dev, blockno);

  acquire(&(hash_table[slot].lock));
  block->refcnt -= 1;
  if (block->refcnt < 0) panic("hash_release: ref cnt < 0");

  if (block->refcnt == 0) {
    // keep: here no other process has this block
    list_erase(block); 
    block_free(slot, block); // lock hashtable -> freetable
  }
  release(&(hash_table[slot].lock));
}

/* Hash Table END */

// void
// binit(void)
// {
//   struct buf *b;

//   initlock(&bcache.lock, "bcache");

//   // Create linked list of buffers
//   bcache.head.prev = &bcache.head; // shall remove
//   bcache.head.next = &bcache.head;
//   for(b = bcache.buf; b < bcache.buf+NBUF; b++){
//     b->next = bcache.head.next;
//     b->prev = &bcache.head;
//     initsleeplock(&b->lock, "buffer");
//     bcache.head.next->prev = b;
//     bcache.head.next = b;
//   }
// }

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->next = b->prev = NULL;
    b->refcnt = 0;
  }

  // init hashtables
  init_free_table();
  init_hash_table();
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// static struct buf*
// bget(uint dev, uint blockno)
// {
//   struct buf *b;

//   acquire(&bcache.lock);

//   // Is the block already cached?
//   for(b = bcache.head.next; b != &bcache.head; b = b->next){
//     if(b->dev == dev && b->blockno == blockno){
//       b->refcnt++;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }

//   // Not cached.
//   // Recycle the least recently used (LRU) unused buffer.
//   for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
//     if(b->refcnt == 0) {
//       b->dev = dev;
//       b->blockno = blockno;
//       b->valid = 0;
//       b->refcnt = 1;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }
//   panic("bget: no buffers");
// }

static struct buf*
bget(uint dev, uint blockno)
{
  Iterator block = hash_get(dev, blockno);
  acquiresleep(&block->lock);
  return block;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
// void
// brelse(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("brelse");

//   releasesleep(&b->lock);

//   acquire(&bcache.lock);
//   b->refcnt--;
//   if (b->refcnt == 0) {
//     // no one is waiting for it.
//     b->next->prev = b->prev;
//     b->prev->next = b->next;
//     b->next = bcache.head.next;
//     b->prev = &bcache.head;
//     bcache.head.next->prev = b;
//     bcache.head.next = b;
//   }
  
//   release(&bcache.lock);
// }

void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");
  releasesleep(&b->lock);
  hash_release(b); // here we take hash_table's lock
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


