#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "lib/kernel/hash.h"
#include "devices/timer.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"

#define MAX 300

static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void (**eip)(void), void **esp);
/*global variable to store the target thread id*/
static tid_t target_tid;
/*global variable to sore the target thread*/
static struct thread *target_t;
static void search_thread(struct thread *t, void *aux);
struct file_to_fd
{
  int f_des;               /*file descriptor*/
  struct file *f_addr_ptr; /*file address*/
  struct list_elem f_list; /*fd list of thread*/
};
struct execute_status
{
  const char *ELF;               /*program to execute*/
  struct semaphore waiting_exec; /*for parent to wait children to load*/
  struct last_words *son_info;    /*son's info*/
  bool load_success;             /*children successfully loaded*/
};
/*search thread by tid, wrapped in order to use foreach()*/
static void
search_thread(struct thread *t, void *aux UNUSED)
{
  if (target_tid == t->tid)
  {
    target_t = t;
  }
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t process_execute(const char *file_name)
{
  char name[16];
  char *save_ptr;
  tid_t tid;
  struct execute_status execute;
  struct thread *cur = thread_current();
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  execute.ELF = file_name;
  sema_init(&execute.waiting_exec, 0);
  strlcpy(name, file_name, sizeof name);
  /*create a new string to store the program name*/
  /*strtok_r() refer to 'lib/string.c' line235*/
  strtok_r(name, " ", &save_ptr);
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(name, PRI_DEFAULT, start_process, &execute);

  /*let current thread wait for child thread to finish loading*/
  if (tid != TID_ERROR)
  {
    sema_down(&execute.waiting_exec);
    if (execute.load_success)
    {
      list_push_back(&thread_current()->children, &execute.son_info->ele);
    }
    else
    {
      tid = TID_ERROR;
    }
  }
  /*loading failed*/
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process(void *file_name_)
{
  struct execute_status *file_name = file_name_;
  struct intr_frame if_;
  /*int load_status;*/
  bool success;
  struct thread *curr = thread_current();

  /* Somehow the *curr->page_table cannot be used in malloc. */
  curr->page_table = malloc(sizeof(struct hash));
  if (curr->page_table == NULL)
    printf("page table malloc failed.\n");
  hash_init(curr->page_table, page_number, page_less, NULL);
  /* Initialize interrupt frame and load executable. */
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load(file_name->ELF, &if_.eip, &if_.esp);

  if (success)
  {
    file_name->son_info = thread_current()->son_info = malloc(sizeof *file_name->son_info);
    success = file_name->son_info != NULL;
  }

  /* Initialize wait_status. */
  if (success)
  {
    lock_init(&file_name->son_info->exit_lock);
    file_name->son_info->under_running = 2;
    file_name->son_info->tid = thread_current()->tid;
    sema_init(&file_name->son_info->waiting_exit, 0);
  }

  /* Notify parent thread and clean up. */
  file_name->load_success = success;
  sema_up(&file_name->waiting_exec);
  if (!success)
  {
    thread_exit();
  }
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  /* If load failed, quit. */
  asm volatile("movl %0, %%esp; jmp intr_exit"
               :
               : "g"(&if_)
               : "memory");
  NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(tid_t child_tid UNUSED)
{
  /*temp to store the element when going through*/
  struct list_elem *temp;
  /*message of child thread*/
  struct thread *curr = thread_current();
  /*if there is no children*/
  if (list_empty(&curr->children))
  {
    /*no need to wait*/
    return -1;
  }
  temp = list_begin(&curr->children);
  /*search if the thread given is the child of current process*/
  while (temp != list_end(&curr->children))
  {
    struct last_words *child_info = list_entry(temp, struct last_words, ele);
    /*store the target child process into the message passing station*/
    if (child_info->tid == child_tid)
    {
      int exitcode;
      list_remove(temp);
      sema_down(&child_info->waiting_exit);
      exitcode = child_info->code;
      int new_under_running;
      lock_acquire(&child_info->exit_lock);
      new_under_running = --child_info->under_running;
      lock_release(&child_info->exit_lock);
      if (new_under_running == 0)
      {
        free(child_info);
      }
      return exitcode;
    }
    temp = list_next(temp);
  }
  return -1;
}

/* Free the current process's resources. */
void process_exit(void)
{
  struct thread *cur = thread_current();
  uint32_t *pd;
  struct list_elem *temp;

  /*Process Termination Messages*/
  printf("%s: exit(%d)\n", cur->name, cur->exitcode);

  /* Find the waiting parent and wake it up. */
  if (cur->son_info != NULL) 
    {
      struct last_words *child_infom = cur->son_info;
      child_infom->code = cur->exitcode;
      sema_up (&child_infom->waiting_exit);
      int rencent_under_running;
      lock_acquire(&child_infom->exit_lock);
      rencent_under_running = --child_infom->under_running;
      lock_release(&child_infom->exit_lock);
      if (rencent_under_running == 0)
      {
        free(child_infom);
      }
    }

  /* Clean the son_info when parent exit, aka free all the structs. */
  temp = list_begin (&cur->children);
  while(temp != list_end (&cur->children))
    {
      struct last_words *child_info = list_entry (temp, struct last_words, ele);
      temp = list_remove (temp);
      int new_under_running;
      lock_acquire(&child_info->exit_lock);
      new_under_running = --child_info->under_running;
      lock_release(&child_info->exit_lock);
      if (new_under_running == 0)
      {
        free(child_info);
      }
    }

  /*close the files*/
  struct list_elem *el;
  while (!list_empty(&cur->file_des))
  {
    el = list_pop_front(&cur->file_des);
    struct file_to_fd *lk = list_entry(el, struct file_to_fd, f_list);
    file_close(lk->f_addr_ptr);
    list_remove(el);
    free(lk);
  }
  cur->curr_fd = 0;
  /*re-enable the file to be written*/
  if (cur->FILE != NULL)
  {
    file_allow_write(cur->FILE);
    file_close(cur->FILE);
  }
  struct list_elem *t;
  t = list_begin(&cur->map_list);
  while (t != list_end(&cur->map_list))
  {
    struct map *m = list_entry(t, struct map, elem);
    t = list_next(t);
    unmap(m);
  }

  /* Free the supplementary PT current process owns. */
  timer_msleep(1000);
  page_table_free();


  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
  {
    /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void process_activate(void)
{
  struct thread *t = thread_current();

  /* Activate thread's page tables. */
  pagedir_activate(t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
{
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
{
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void **esp, char *argv[], int argc);
static bool validate_segment(const struct Elf32_Phdr *, struct file *);
static bool load_segment_lazily(struct file *file, off_t ofs, uint8_t *upage,
                                uint32_t read_bytes, uint32_t zero_bytes,
                                bool writable);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char *file_name, void (**eip)(void), void **esp)
{
  struct thread *t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  t->pagedir = pagedir_create();
  if (t->pagedir == NULL)
    goto done;

  process_activate();
  /*create string to store arguments*/
  char *save_ptr;
  char *token;
  /*initialize the argument number*/
  int argc = 0;
  /*create arguement list*/
  char *argv[MAX];
  /*get first arg*/
  token = strtok_r((char *)file_name, " ", &save_ptr);
  while (token != NULL)
  {
    /*save the arg in the list*/
    argv[argc] = token;
    /*update arg number*/
    argc++;
    /*get next arg*/
    token = strtok_r(NULL, " ", &save_ptr);
  }
  /* Open executable file. */
  file = filesys_open(argv[0]);
  if (file == NULL)
  {
    printf("load: %s: open failed\n", argv[0]);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024)
  {
    printf("load: %s: error loading executable\n", argv[0]);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;

  for (i = 0; i < ehdr.e_phnum; i++)
  {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type)
    {
    case PT_NULL:
    case PT_NOTE:
    case PT_PHDR:
    case PT_STACK:
    default:
      /* Ignore this segment. */
      break;
    case PT_DYNAMIC:
    case PT_INTERP:
    case PT_SHLIB:
      goto done;
    case PT_LOAD:

      if (validate_segment(&phdr, file))
      {
        bool writable = (phdr.p_flags & PF_W) != 0;
        uint32_t file_page = phdr.p_offset & ~PGMASK;
        uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
        uint32_t page_offset = phdr.p_vaddr & PGMASK;
        uint32_t read_bytes, zero_bytes;
        if (phdr.p_filesz > 0)
        {
          /* Normal segment.
                     Read initial part from disk and zero the rest. */
          read_bytes = page_offset + phdr.p_filesz;
          zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
        }
        else
        {
          /* Entirely zero.
                     Don't read anything from disk. */
          read_bytes = 0;
          zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
        }

        if (!load_segment_lazily(file, file_page, (void *)mem_page,
                                 read_bytes, zero_bytes, writable))
          goto done;
      }
      else
        goto done;
      break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp, argv, argc))
  {
    goto done;
  }

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  if (success)
  {
    struct thread *curr = thread_current();
    curr->FILE = file;
    file_deny_write(file);
  }
  else
  {
    file_close(file);
  }
  return success;
}

/* load() helpers. */

static bool install_page(void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void *)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* To implement the lazy-load load segment function. */
static bool
load_segment_lazily(struct file *file, off_t ofs, uint8_t *upage,
                    uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0)
  {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    struct page *p = page_allocate(upage, !writable);
    if (p == NULL)
      return false;

    if (page_read_bytes > 0)
    {
      p->file = file;
      p->offset = ofs;
      p->bytes = page_read_bytes;
    }

    /* property maintaining. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    ofs += page_read_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:
        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.
        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.
   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
  {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t *p = palloc_get_page(PAL_USER);
    if (p == NULL)
      return false;

    /* Load this page.*/
    if (file_read(file, p, page_read_bytes) != (int)page_read_bytes)
    {
      page_free(p);
      return false;
    }
    memset(p + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space.*/
    if (!install_page(upage, p, writable))
    {
      page_free(p);
      return false;
    }
    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}
/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack(void **esp, char *argv[], int argc)
{
  struct page *upage;
  bool success = false;

  upage = page_allocate(((uint8_t *)PHYS_BASE) - PGSIZE, false);
  if (upage != NULL)
  {
    upage->frame = frame_allocate(upage);

    if (upage->frame != NULL)
    {

      install_page(upage->addr, upage->frame->ker_base, true);
      /* The first page of stack stores the para etc. so should not
        be swapped or written to change. */
      upage->swapable = false;
      upage->writable = false;

      *esp = PHYS_BASE;
      /*address to argument that put in stack initially*/
      uint32_t *arg_addr[argc];
      int length;
      int total_length = 0;

      /*add argument in command and program into stack from back*/
      for (int i = argc - 1; i >= 0; i--)
      {
        /*allocate memory for str(add '\0' at last)*/
        *esp = *esp - sizeof(char) * (strlen(argv[i]) + 1);
        length = sizeof(char) * (strlen(argv[i]) + 1);
        memcpy(*esp, argv[i], sizeof(char) * (strlen(argv[i]) + 1));
        arg_addr[i] = (uint32_t *)*esp;
        total_length = total_length + length;
      }

      *esp -= 4 - total_length % 4;
      /*allocate for sentinel*/
      *esp = *esp - 4;
      /*add sentinel into stack*/
      (*(int *)(*esp)) = 0;
      *esp = *esp - 4;

      for (int i = argc - 1; i >= 0; i--)
      {
        /* allocate space and push address of arguement 
          stored in arry before*/
        (*(uint32_t **)(*esp)) = arg_addr[i];
        *esp = *esp - 4;
      }

      /*allocate and push pointer to argv into stack*/
      (*(uintptr_t **)(*esp)) = *esp + 4;
      /*allocate and push number of argument into stack*/
      *esp = *esp - 4;
      *(int *)(*esp) = argc;
      /*allocate and push fake "return adress" into stack*/
      *esp = *esp - 4;
      (*(int *)(*esp)) = 0;
      frame_unlock(upage->frame);
      success = true;
    }
  }

  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pagedir, upage) == NULL && pagedir_set_page(t->pagedir, upage, kpage, writable));
}
