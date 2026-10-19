#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);


struct child_status {
  tid_t tid;
  int exit_code;              
  bool load_ok;
  bool waited;
  int ref_cnt;                
  struct semaphore load_sema;  
  struct semaphore exit_sema;  
  struct lock lock;            
  struct list_elem elem;
};

struct exec_args {
  char *cmdline;               
  struct child_status *cs;
};

void release_status (struct child_status *cs);

#endif /**< userprog/process.h */
