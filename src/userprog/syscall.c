#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "lib/syscall-nr.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "threads/thread.h"
#include "threads/synch.h"   /* later, for the filesystem lock */
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include "threads/malloc.h"



static void sys_exit(int status);


// Helpers for handling file descriptors

static struct file_descriptor *find_fd(int fd);
static int add_fd (struct file *file);
static void remove_fd(int fd);
static struct file *get_file (int fd);
static void check_string (const char *string);


static struct lock filesys_lock; 

static bool valid_uaddr(const void *p) {
  return p != NULL && is_user_vaddr(p)
      && pagedir_get_page(thread_current()->pagedir, p) != NULL;
}

/* Check every byte range of a user buffer. */
static void check_buffer(const void *buf, unsigned size) {
  for (unsigned i = 0; i < size; i++)
    if (!valid_uaddr((const char *)buf + i))
      sys_exit(-1);
}

/* Read the Nth 32-bit argument from the user stack, validating it. */
static uint32_t get_arg(const uint32_t *esp, int n) {
  if (!valid_uaddr(esp + n) || !valid_uaddr((const char *)(esp + n) + 3))
    sys_exit(-1);
  return esp[n];
}

static void sys_exit(int status) {
  #ifdef USERPROG
  thread_exit_with_status(status);
  #endif
  thread_exit();
  
}

static void syscall_handler(struct intr_frame *f) {
  const uint32_t *esp = f->esp;
  int number = get_arg(esp, 0);

  switch (number) {
    case SYS_HALT:
      shutdown_power_off();
      break;

    case SYS_EXIT:
      sys_exit((int)get_arg(esp, 1));
      break;

    case SYS_WAIT:
      f->eax = process_wait ((tid_t) get_arg (esp, 1));
      break;

    case SYS_EXEC: {
      const char *cmd = (const char *) get_arg (esp, 1);
      /* Validate every byte of the string, up to and including the '\0'. */
      for (const char *p = cmd; ; p++)
        {
          if (!valid_uaddr (p))
            sys_exit (-1);          /* or your thread_exit_with_status(-1) */
          if (*p == '\0')
            break;
        }
      f->eax = process_execute (cmd);
      break;
    }
    case SYS_CREATE:
    {
      const char *name = (const char *) get_arg (esp, 1);
      unsigned initial_size = get_arg (esp, 2);

      check_string (name);

      lock_acquire (&filesys_lock);
      f->eax = filesys_create (name, initial_size);
      lock_release (&filesys_lock);
      break;
    }

    case SYS_OPEN:
    {
      const char *name = (const char *) get_arg (esp, 1);
      check_string (name);

      lock_acquire (&filesys_lock);
      struct file *file = filesys_open (name);
      lock_release (&filesys_lock);

      if (file == NULL)
        {
          f->eax = -1;
          break;
        }

      f->eax = add_fd (file);

      if ((int) f->eax == -1)
        {
          lock_acquire (&filesys_lock);
          file_close (file);
          lock_release (&filesys_lock);
        }

      break;
    }

    case SYS_CLOSE:
    {
      int fd = (int) get_arg (esp, 1);
      remove_fd (fd);
      break;
    }

    case SYS_READ:
    {
      int fd = (int) get_arg (esp, 1);
      void *buffer = (void *) get_arg (esp, 2);
      unsigned size = get_arg (esp, 3);

      check_buffer (buffer, size);

      if (fd == 0)
        {
          unsigned i;

          for (i = 0; i < size; i++)
            ((uint8_t *) buffer)[i] = input_getc ();

          f->eax = size;
        }
      else if (fd == 1)
        {
          f->eax = -1;
        }
      else
        {
          struct file *file = get_file (fd);

          if (file == NULL)
            f->eax = -1;
          else
            {
              lock_acquire (&filesys_lock);
              f->eax = file_read (file, buffer, size);
              lock_release (&filesys_lock);
            }
        }

      break;
    }

    case SYS_WRITE:
    {
      int fd = (int) get_arg (esp, 1);
      const void *buffer = (const void *) get_arg (esp, 2);
      unsigned size = get_arg (esp, 3);

      check_buffer (buffer, size);

      if (fd == 1)
        {
          putbuf (buffer, size);
          f->eax = size;
        }
      else if (fd == 0)
        {
          f->eax = -1;
        }
      else
        {
          struct file *file = get_file (fd);

          if (file == NULL)
            f->eax = -1;
          else
            {
              lock_acquire (&filesys_lock);
              f->eax = file_write (file, buffer, size);
              lock_release (&filesys_lock);
            }
        }

      break;
    }

    case SYS_REMOVE:
    {
      const char *name = (const char *) get_arg (esp, 1);
      check_string (name);

      lock_acquire (&filesys_lock);
      f->eax = filesys_remove (name);
      lock_release (&filesys_lock);
      break;
    }

    case SYS_FILESIZE:
    {
      struct file *file = get_file ((int) get_arg (esp, 1));

      if (file == NULL)
        f->eax = -1;
      else
        {
          lock_acquire (&filesys_lock);
          f->eax = file_length (file);
          lock_release (&filesys_lock);
        }

      break;
    }

    case SYS_SEEK:
    {
      struct file *file = get_file ((int) get_arg (esp, 1));
      unsigned position = get_arg (esp, 2);

      if (file != NULL)
        {
          lock_acquire (&filesys_lock);
          file_seek (file, position);
          lock_release (&filesys_lock);
        }

      break;
    }

    case SYS_TELL:
    {
      struct file *file = get_file ((int) get_arg (esp, 1));

      if (file == NULL)
        f->eax = -1;
      else
        {
          lock_acquire (&filesys_lock);
          f->eax = file_tell (file);
          lock_release (&filesys_lock);
        }

      break;
    }
    default:
      thread_exit_with_status(-1);
  }
}

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}


// Implementation of file descriptor management functions

static struct file_descriptor *find_fd(int fd){
  struct thread *cur = thread_current();
  for (struct list_elem *e = list_begin(&cur->open_files); e != list_end(&cur->open_files); e = list_next(e)) {
    struct file_descriptor *fd_struct = list_entry(e, struct file_descriptor, elem);
    if (fd_struct->fd == fd) {
      return fd_struct;
    }
  }
  return NULL; // Not found
}

static int add_fd (struct file *file){
  struct thread *cur = thread_current();
  struct file_descriptor *new_fd = malloc(sizeof(struct file_descriptor));
  if (new_fd == NULL) {
    return -1; // Allocation failed
  }
  new_fd->fd = cur->next_fd++;
  new_fd->file = file;
  list_push_back(&cur->open_files, &new_fd->elem);
  return new_fd->fd;
}

static void
remove_fd (int fd)
{
  struct file_descriptor *descriptor = find_fd (fd);

  if (descriptor == NULL)
    return;

  lock_acquire (&filesys_lock);
  file_close (descriptor->file);
  lock_release (&filesys_lock);

  list_remove (&descriptor->elem);
  free (descriptor);
}

void
close_all_fds (void)
{
  struct thread *current = thread_current ();

  while (!list_empty (&current->open_files))
    {
      struct list_elem *element = list_pop_front (&current->open_files);
      struct file_descriptor *descriptor =
        list_entry (element, struct file_descriptor, elem);

      lock_acquire (&filesys_lock);
      file_close (descriptor->file);
      lock_release (&filesys_lock);

      free (descriptor);
    }
}

static struct file *
get_file (int fd)
{
  struct file_descriptor *descriptor = find_fd (fd);

  if (descriptor == NULL)
    return NULL;

  return descriptor->file;
}

static void
check_string (const char *string)
{
  if (string == NULL)
    sys_exit (-1);

  while (true)
    {
      if (!valid_uaddr (string))
        sys_exit (-1);

      if (*string == '\0')
        return;

      string++;
    }
}