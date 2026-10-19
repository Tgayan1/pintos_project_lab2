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
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "userprog/syscall.h"


#define MAX_ARGS 128

/*
Freeing Allocated Child Status Structs when the reference count reaches 0. 
This is called when a child process exits and when the parent process calls process_wait() on the child. 
The reference count is decremented in both cases, and when it reaches 0, the struct is freed.
*/
void
release_status (struct child_status *cs)
{
  lock_acquire (&cs->lock);
  int n = --cs->ref_cnt;
  lock_release (&cs->lock);
  if (n == 0)
    free (cs);
}


static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

// Helper for Building User stack.
static bool push_args(void **esp, char *argv[], int argc);

/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *cmd_line) 
{
  char cmd_copy[16];
  strlcpy(cmd_copy, cmd_line, sizeof cmd_copy);

  char *cln_copy;
  tid_t tid;

  struct child_status *cs;
  struct exec_args ea;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  cln_copy = palloc_get_page (0);
  if (cln_copy == NULL)
    return TID_ERROR;
  strlcpy (cln_copy, cmd_line, PGSIZE);

  // Initialize child status struct
  cs = malloc(sizeof(struct child_status));

  if(cs == NULL){
    palloc_free_page(cln_copy);
    return TID_ERROR;
  }

  cs->tid = TID_ERROR;
  cs->exit_code = -1;
  cs->load_ok = false;
  cs->waited = false;
  cs->ref_cnt = 2; // One for parent, one for child
  sema_init(&cs->load_sema, 0);
  sema_init(&cs->exit_sema, 0);
  lock_init(&cs->lock);

  ea.cmdline = cln_copy;
  ea.cs = cs;

  char *temp;
  /* Create a new thread to execute PRG_NAME (splitted from CMDLINE). */
  tid = thread_create (strtok_r(cmd_copy, " ", &temp), PRI_DEFAULT, start_process, &ea);

  if (tid == TID_ERROR){
    palloc_free_page (cln_copy); 
    free(cs); // Free the child status struct if thread creation fails
    return TID_ERROR;
  }
  
  // Wait for the child to load and check if it was successful  
  sema_down(&cs->load_sema);
  if (!cs->load_ok) {
    release_status(cs);
    return TID_ERROR;
  }

  // Add the child status struct to the parent's list of children
  list_push_back(&thread_current()->children, &cs->elem);

  return tid;
}

/** A thread function that loads a user process and starts it
   running. */
static void
start_process (void *excec_args_)
{
  // Extract the command line and child status struct from the exec_args struct
  struct exec_args *ea = (struct exec_args *)excec_args_;
  char *cmd_line = ea->cmdline;
  struct child_status *cs = ea->cs;

  struct intr_frame if_;
  bool success;

  // Set the current thread's child status struct to the one passed in
  thread_current()->my_status = cs;
  cs->tid = thread_current()->tid;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (cmd_line, &if_.eip, &if_.esp);


  palloc_free_page (cmd_line);

  // Set the load_ok flag in the child status struct and signal the parent that loading is complete
  cs->load_ok = success;
  sema_up(&cs->load_sema);

  /* If load failed, quit. */
  if (!success){ 
    thread_exit_with_status(-1);
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/** Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid)
{
  struct thread *cur = thread_current ();
  struct child_status *cs = NULL;

  /* Find child_tid among MY children. */
  for (struct list_elem *e = list_begin (&cur->children);
       e != list_end (&cur->children);
       e = list_next (e))
    {
      struct child_status *c = list_entry (e, struct child_status, elem);
      if (c->tid == child_tid)
        {
          cs = c;
          break;
        }
    }

  if (cs == NULL || cs->waited)
    return -1;

  cs->waited = true;
  sema_down (&cs->exit_sema);   /* returns at once if the child already exited */
  return cs->exit_code;
}

/** Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  struct child_status *cs = cur->my_status;
  uint32_t *pd;

  // If the current thread has a child status struct, 
  // print the exit message and signal the parent that the child has exited.
  if (cs != NULL)
    {
      printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
      cs->exit_code = cur->exit_status;
      sema_up (&cs->exit_sema);          /* wake a waiting parent */
    }

  // Free all child_status structs in the current thread's children list.
  while (!list_empty (&cur->children))
    {
      struct list_elem *e = list_pop_front (&cur->children);
      release_status (list_entry (e, struct child_status, elem));
    }

  // Close all open file descriptors
  close_all_fds();

  // If the current thread has an executable file, allow write and close it.
  if (cur->executable != NULL)
  {
    file_allow_write (cur->executable);
    file_close (cur->executable);
    cur->executable = NULL;
  }

  // Destroy the current thread's page directory and switch back to the kernel-only page directory.
  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }

  // Release the current thread's child status struct if it exists.
  if (cs != NULL)
    release_status (cs);

   
}

/** Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/** We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/** ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** For use with ELF types in printf(). */
#define PE32Wx PRIx32   /**< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /**< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /**< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /**< Print Elf32_Half in hexadecimal. */

/** Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/** Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/** Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /**< Ignore. */
#define PT_LOAD    1            /**< Loadable segment. */
#define PT_DYNAMIC 2            /**< Dynamic linking info. */
#define PT_INTERP  3            /**< Name of dynamic loader. */
#define PT_NOTE    4            /**< Auxiliary info. */
#define PT_SHLIB   5            /**< Reserved. */
#define PT_PHDR    6            /**< Program header table. */
#define PT_STACK   0x6474e551   /**< Stack segment. */

/** Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /**< Executable. */
#define PF_W 2          /**< Writable. */
#define PF_R 4          /**< Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/** Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *cmd_line, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  char *cmd_cpy = cmd_line;
  char *save_ptr;
  char *argv[MAX_ARGS];
  int argc = 0;

  char *token = strtok_r(cmd_cpy, " ", &save_ptr);
  while (token != NULL && argc < MAX_ARGS) {
    argv[argc++] = token;
    token = strtok_r(NULL, " ", &save_ptr);
  }

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (argv[0]);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", argv[0]);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", argv[0]);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
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
          if (validate_segment (&phdr, file)) 
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
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  if(!push_args(esp,argv,argc)){
    goto done;
  }

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  // Deny write to the executable file to prevent it from being modified while running
  if(success){
    file_deny_write (file);
    thread_current()->executable = file; 
    file = NULL;
  }
  
  if(file != NULL)
    file_close (file);

  // // debug line
  // /*-------------------------------------------------------*/
  // hex_dump((uintptr_t)*esp, *esp, PHYS_BASE - *esp, true);

  return success;
}

/** load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/** Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
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

/** Loads a segment starting at offset OFS in FILE at address
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
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/** Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/** Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

static bool push_args(void **esp, char *argv[], int argc) {
  char *addr[MAX_ARGS];

  
  size_t total_size = 0;
  // Sum of Tokens
  for (int i = 0; i < argc; i++) {
    total_size += strlen(argv[i]) + 1;  
  }
  // Alignment
  total_size += total_size % 4;

  // pointer array argv[] + argv + argc
  total_size += (argc + 1) * sizeof(char *);
  total_size += sizeof(int);

  // fake return address
  total_size += sizeof(void *); 

  if(total_size > PGSIZE){
    return false;
  }

  /* copy the string to user stack */
  for (int i = argc - 1; i >= 0; i--) {
    size_t len = strlen(argv[i]) + 1;      
    *esp -= len;
    memcpy(*esp, argv[i], len);
    addr[i] = *esp;                          
  }

  /* alignment (by multiple of 4 using ~3)*/
  *esp = (void *)((uintptr_t)*esp & ~3);

  /*NULL Sentinal*/
  *esp -= sizeof(char *);
  *(char **)*esp = NULL;

  /* argv pointers */
  for (int i = argc - 1; i >= 0; i--) {
    *esp -= sizeof(char *);
    *(char **)*esp = addr[i];
  }

  /* argv, argc and fake return */
  char **argv_ptr = *esp;
  *esp -= sizeof(char **);
  *(char ***)*esp = argv_ptr;

  *esp -= sizeof(int);
  *(int *)*esp = argc;

  *esp -= sizeof(void *);
  *(void **)*esp = NULL;

  return true;
}