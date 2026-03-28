//
//  hook.h
//  iSH
//
//  Created by Saagar Jha on 12/29/22.
//

#ifndef hook_h
#define hook_h

#include <stdbool.h>

void *find_symbol(void *base, char *symbol);
bool hook(void *old, void *new);

// Install EXC_BAD_ACCESS handler on the calling thread for JIT crash recovery.
// Must be called once per JIT execution thread.  Safe to call before any hook()
// call; starts the exception server if not already running.
void jit_install_thread_exception_handler(void);

#endif /* hook_h */
