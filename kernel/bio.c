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

#define HASHSIZE 13

struct {
  struct spinlock headlk[HASHSIZE], freelk[NCPU];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf heads[HASHSIZE], freelist[NCPU];
} bcache;

void
binit(void)
{
  struct buf *b;


  // Create linked list of buffers
  for(int i = 0; i < HASHSIZE; ++i) {
  	initlock(&bcache.headlk[i], "bcache.bucket");
		bcache.heads[i].prev = &bcache.heads[i];
		bcache.heads[i].next = &bcache.heads[i];
	}
	for(int i = 0; i < NCPU; ++i) {
		initlock(&bcache.freelk[i], "bcache.freelist");
		bcache.freelist[i].next = bcache.freelist[i].prev = &bcache.freelist[i];
	}
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.freelist[0].next;
    b->prev = &bcache.freelist[0];
    initsleeplock(&b->lock, "buffer");
    bcache.freelist[0].next->prev = b;
    bcache.freelist[0].next = b;
		b->timestamp = ticks;
  }
}

int hash(uint dev, uint blockno) { return (1234 * dev + 5678 * blockno + 90) % HASHSIZE; }

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

	uint id = hash(dev, blockno);
  acquire(&bcache.headlk[id]);

  // Is the block already cached?
  for(b = bcache.heads[id].next; b != &bcache.heads[id]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
			b->timestamp = ticks;
      release(&bcache.headlk[id]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
	push_off();
	int cpu = cpuid();
	acquire(&bcache.freelk[cpu]);
	for(b = bcache.freelist[cpu].prev; b != &bcache.freelist[cpu]; b = b->prev) {
		if(b->refcnt) panic("bget: freelist is not free!");
		b->prev->next = b->next;
		b->next->prev = b->prev;
		b->next = bcache.heads[id].next;
		b->prev = &bcache.heads[id];
		bcache.heads[id].next->prev = b;
		bcache.heads[id].next = b;
		b->dev = dev;
		b->blockno = blockno;
		b->valid = 0;
		b->refcnt = 1;
		release(&bcache.headlk[id]);
		release(&bcache.freelk[cpu]);
		acquiresleep(&b->lock);
		pop_off();
		return b;
	}
	release(&bcache.freelk[cpu]);
	for(int nc = 0; nc < NCPU; ++nc) if(nc != cpu) {
			acquire(&bcache.freelk[nc]);
			for(b = bcache.freelist[nc].prev; b != &bcache.freelist[nc]; b = b->prev) {
				if(b->refcnt) panic("bget: freelist is not free!");
				b->prev->next = b->next;
				b->next->prev = b->prev;
				b->next = bcache.heads[id].next;
				b->prev = &bcache.heads[id];
				bcache.heads[id].next->prev = b;
				bcache.heads[id].next = b;
				b->dev = dev;
				b->blockno = blockno;
				b->valid = 0;
				b->refcnt = 1;
				release(&bcache.headlk[id]);
				release(&bcache.freelk[nc]);
				acquiresleep(&b->lock);
				pop_off();
				return b;
			}
			release(&bcache.freelk[nc]);
	}
	pop_off();
  panic("bget: no buffers");
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
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

	int id = hash(b->dev, b->blockno);
  acquire(&bcache.headlk[id]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
		push_off();
		int cpu = cpuid();
		acquire(&bcache.freelk[cpu]);
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.freelist[cpu].next;
    b->prev = &bcache.freelist[cpu];
    bcache.freelist[cpu].next->prev = b;
    bcache.freelist[cpu].next = b;
		release(&bcache.freelk[cpu]);
		pop_off();
  }
  
  release(&bcache.headlk[id]);
}
void
bpin(struct buf *b) {
  int id = hash(b->dev, b->blockno);
  acquire(&bcache.headlk[id]);
  b->refcnt++;
  release(&bcache.headlk[id]);
}

void
bunpin(struct buf *b) {
  int id = hash(b->dev, b->blockno);
  acquire(&bcache.headlk[id]);
  b->refcnt--;
  release(&bcache.headlk[id]);
}

