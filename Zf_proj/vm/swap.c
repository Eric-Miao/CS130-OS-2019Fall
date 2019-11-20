#include "vm/swap.h"
#include <bitmap.h>
#include <debug.h>
#include <stdio.h>
#include "vm/frame.h"
#include "vm/page.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/block.h"

/* The swap block on disk. */
struct  block *swap;

/* The lock for swap. */
struct lock swap_lock;

/* The bitmap used to manage the swap. */
struct bitmap swap_bitmap;

/* The sector number per page, calculated by the division. */
#define SECTOR_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

void
swap_init()
{
    size_t swap_size;

    /* Initialize a block on disk of swap type. */
    swap = block_get_role(BLOCK_SWAP);
    lock_init(&swap_lock);
    if (swap)
    {
        /* Thus one bit indicates several sectors needed for complete page. */
        swap_size = block_size(swap_bitmap) / SECTOR_PER_PAGE;
        swap_bitmap = bitmap_create(swap_size);
        if(!swap_bitmap)
        {
            printf("swap bitmap creation failed.\n")
        }
    }
    else
    {
        printf("swap initialization failed.\n");
    }

}

/* To swap a page p into swap block, which is write into disk.
    Return true if swap succeeed, false otherwise.*/
bool 
swap_page_into_disk(struct page *p)
{
    size_t write_pos;
    block_sector_t base_to_wrtie;
    void* base_to_read;

    if(!is_valid_swap(p, false))
        return false;
    
    /* Find a sector on disk to write. */
    write_pos = bitmap_scan(swap_bitmap, 0, 1, false);
    if (write_pos == BITMAP_ERROR)
    {
        printf("bitmap full to write.\n")
        return false;
    }
    bitmap_flip(swap_bitmap, write_pos);
    base_to_wrtie = write_pos * SECTOR_PER_PAGE;
    base_to_read = p->frame->base;

    /* Write. */
    /* From the start of the found sector, to the end of this bit in bitmap
        which is just the size of secotrs in a page. 
        Each time write a BlOCK_SECOTR_SIZE of bytes from frame into swap,
        whic is the buffer (the start point of the next loop to write.*/
    for (int i = 0; i < SECTOR_PER_PAGE; i++)
        block_write(swap, base_to_wrtie + i,
                    base_to_read + i * BLOCK_SECTOR_SIZE);
    /* Reset the page/frame's property */
    p->swap_sector = base_to_wrtie;
    p->file = NULL;
    
    /* Not sure the usage of the following properties, check later. */
    p->file_offset = 0;
    p->file_bytes = 0;

    return true;
}

bool  
swap_disk_outto_page(page *p)
{
    size_t read_pos;
    block_sector_t base_to_read;
    void* base_to_write;

    if (!is_valid_swap(p, true))
        return false;

    /* locate the sector in swap to read. */
    read_pos = p->sector/SECTOR_PER_PAGE;
    base_to_read = page->sector;
    base_to_write = frame->base

    /* Read. */
    for(int i = 0; i < SECTOR_PER_PAGE; i++)
        block_read(swap, base_to_read + i, 
                    base_to_write + i * BLOCK_SECTOR_SIZE);

    /* Reset the bitmap. */
    bitmap_reset(swap_bitmap, read_pos);
    
    return true;
}
/*  The read indicate if this is a read swap into page, false otherwise.
    If a process wants to read/write a page into swap,
    1. The page shall have a physical frame in map.
    2. The process shall have the frams's lock to ocuupy the frame.
    3. If it's a read, the frame shall be prewritten into swap. */
static bool
is_valid_swap(struct page* p, bool read)
{
    if (p->frame == NULL)
        return false;
    if (!lock_held_by_current_thread(p->frame->frame_lock))
        return false;
    if (read && p->sector == (block_sector_t)-1)
        return false;
    return true;
}