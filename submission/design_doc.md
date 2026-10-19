# Project 2: User Programs

## Preliminaries

>Fill in your name and email address.

Tharindu Gayan 

>If you have any preliminary comments on your submission, notes for the TAs, please give them here.


>Please cite any offline or online sources you consulted while preparing your submission, other than the Pintos documentation, course text, lecture notes, and course staff.


## Argument Passing

#### DATA STRUCTURES

>A1: Copy here the declaration of each new or changed struct or struct member, global or static variable, typedef, or enumeration.  Identify the purpose of each in 25 words or less.


#### ALGORITHMS

>A2: Briefly describe how you implemented argument parsing.  How do you arrange for the elements of argv[] to be in the right order?

I used strtok_r() to split the command line into individual arguments and stored the resulting tokens in argv[] in their original order. When constructing the user stack, I copied the argument strings from last to first, because the stack grows downward. I recorded each string’s stack address, then pushed the addresses in reverse order so that argv[0] points to the program name, followed by argv[1], argv[2], and so on. Finally, I pushed the argv pointer, argc, and a fake return address.

>How do you avoid overflowing the stack page?

I avoid overflowing the stack page by calculating the total space required before placing anything on the stack. This includes the argument strings, null terminators, alignment padding, the argv[] pointers, the argv pointer, argc, and the fake return address. Since the stack grows downward from PHYS_BASE, the arguments are copied only if the total size fits within PGSIZE (one page). If the required space is larger than one page, push_args() returns false and the process load fails instead of writing outside the allocated stack page. The maximum number of arguments is also limited using MAX_ARGS.


#### RATIONALE

>A3: Why does Pintos implement strtok_r() but not strtok()?

strtok_r() stores parsing state in a caller-provided variable instead of using hidden global state. This makes it reentrant and safer when parsing can be nested or performed by multiple threads.

>A4: In Pintos, the kernel separates commands into a executable name and arguments.  In Unix-like systems, the shell does this separation.  Identify at least two advantages of the Unix approach.

The shell can perform command history, pipes, redirection, wildcard expansion, and environment-variable expansion.

Programs receive already-separated arguments and do not need to implement command-line parsing.

## System Calls

#### DATA STRUCTURES

>B1: Copy here the declaration of each new or changed struct or struct member, global or static variable, typedef, or enumeration.  Identify the purpose of each in 25 words or less.



>B2: Describe how file descriptors are associated with open files. Are file descriptors unique within the entire OS or just within a single process?



#### ALGORITHMS

>B3: Describe your code for reading and writing user data from the kernel.

struct child_status: Shared parent-child record containing load status, exit status, semaphores, reference count, and wait state.

struct exec_args: Arguments passed from process_execute() to the new thread.

struct file_descriptor: Associates a process-local file descriptor with a struct file.

struct thread.open_files: List of files opened by the current process.

struct thread.next_fd: Next file descriptor number allocated by the process.

struct thread.executable: File currently being executed, kept open to deny writes.

filesys_lock: Global lock protecting file-system and file operations.


>B4: Suppose a system call causes a full page (4,096 bytes) of data
>to be copied from user space into the kernel.  What is the least
>and the greatest possible number of inspections of the page table
>(e.g. calls to pagedir_get_page()) that might result?  What about
>for a system call that only copies 2 bytes of data?  Is there room
>for improvement in these numbers, and how much?

With the current byte-by-byte validation:

A 4,096-byte buffer causes 4,096 page-table inspections.
A 2-byte buffer causes 2 page-table inspections.
This could be improved by validating one address per page instead of one address per byte. A full-page buffer would then require one inspection when page-aligned, or two if it crosses a page boundary. A two-byte buffer would require one inspection when it remains within one page, or two if it crosses a page boundary.

>B5: Briefly describe your implementation of the "wait" system call
>and how it interacts with process termination.

process_wait() searches the current process's child list for the requested child. If the child is not a child of the caller, or has already been waited on, it returns -1.

Each child has an exit_sema. The child calls sema_up() in process_exit() after storing its exit status. The parent calls sema_down() in process_wait(), so it blocks until the child exits. If the child exited first, the semaphore value allows the parent to continue immediately.

The shared child_status record is reference-counted so it remains valid while either the parent or child still needs it.

>B6: Any access to user program memory at a user-specified address
>can fail due to a bad pointer value.  Such accesses must cause the
>process to be terminated.  System calls are fraught with such
>accesses, e.g. a "write" system call requires reading the system
>call number from the user stack, then each of the call's three
>arguments, then an arbitrary amount of user memory, and any of
>these can fail at any point.  This poses a design and
>error-handling problem: how do you best avoid obscuring the primary
>function of code in a morass of error-handling?  Furthermore, when
>an error is detected, how do you ensure that all temporarily
>allocated resources (locks, buffers, etc.) are freed?  In a few
>paragraphs, describe the strategy or strategies you adopted for
>managing these issues.  Give an example.

The implementation separates validation from system-call operations. get_arg() validates system-call arguments, check_string() validates strings through their null terminator, and check_buffer() validates every byte of a buffer.

File-system locks are acquired only after pointer validation and are released immediately after the protected operation. This prevents a process terminated for an invalid pointer from leaving the global file-system lock held.

For example, write() validates its descriptor, buffer, and size before acquiring filesys_lock. If the descriptor is invalid, it returns -1; if the buffer is invalid, the process exits with status -1.

#### SYNCHRONIZATION

>B7: The "exec" system call returns -1 if loading the new executable
>fails, so it cannot return before the new executable has completed
>loading.  How does your code ensure this?  How is the load
>success/failure status passed back to the thread that calls "exec"?

process_execute() creates a child_status record containing load_sema and load_ok. The parent calls sema_down(&cs->load_sema) immediately after creating the child.

The child loads the executable, stores the result in cs->load_ok, and calls sema_up(&cs->load_sema). Therefore, exec() does not return until loading has completed. It returns the child's PID on success or -1 when loading fails.


>B8: Consider parent process P with child process C.  How do you
>ensure proper synchronization and avoid race conditions when P
>calls wait(C) before C exits?  After C exits?  How do you ensure
>that all resources are freed in each case?  How about when P
>terminates without waiting, before C exits?  After C exits?  Are
>there any special cases?

The parent and child share a child_status record. The parent owns one reference and the child owns another.

If the parent calls wait() before the child exits, it blocks on exit_sema. When the child exits, it stores its exit status and signals the semaphore.

If the child exits first, its signal remains available and a later wait() returns immediately. The waited flag prevents a second successful wait on the same child.

If the parent exits before the child, it releases its reference to the child-status record. The child keeps its reference and can still finish safely. If the child exits first, it releases its reference and the parent can later retrieve the stored result.

After a successful wait, the parent should remove the child record from its list and release its reference. This cleanup should be added if it is not already present.

#### RATIONALE

>B9: Why did you choose to implement access to user memory from the
>kernel in the way that you did?

I chose explicit validation with pagedir_get_page() because it makes invalid user addresses fail before the kernel dereferences them. It is easier to reason about and ensures that system calls terminate the offending process instead of panicking the kernel.

The disadvantage is that byte-by-byte buffer validation performs many page-table inspections. A page-based validation strategy would reduce this overhead.


>B10: What advantages or disadvantages can you see to your design
>for file descriptors?

Per-process descriptor lists are simple and match Pintos's requirement that descriptors are process-local. They make descriptor lookup, closing, and cleanup straightforward, and they prevent child processes from inheriting parent descriptors.

The disadvantages are linear lookup time and additional allocation for every open descriptor. A table indexed by descriptor number could provide faster lookup but would require more bookkeeping.

>B11: The default tid_t to pid_t mapping is the identity mapping.
>If you changed it, what advantages are there to your approach?

The identity mapping is simple because the PID returned to the user is the same identifier used internally by the thread system. It avoids a translation table and makes exec() and wait() straightforward.

The disadvantage is that internal thread identifiers become part of the user-visible interface. A separate PID namespace would provide better abstraction and allow internal thread IDs to change independently, but it would require an additional mapping and cleanup mechanism.