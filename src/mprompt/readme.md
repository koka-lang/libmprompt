# Libmprompt Sources

- `main.c`: includes all needed sources in one file for easy inclusion into other projects.
- `mprompt.c`: the main multi-prompt delimited control interface. Uses the lower-level
   _gstacks_ and _longjmp_ assembly routines.
- `gstack.c`: in-place growable stacks using virtual memory. Provides the main interface
  to allocate gstacks and maintains a thread-local cache of gstacks.
  This file includes the  following files depending on the OS:
   - `gstack_gpool.c`: Implements an efficient virtual "pool" of gstacks that is needed
      on systems without overcommit to reliably (and efficiently) determine if an address can be 
      demand-paged.
   - `gstack_win.c`: gstacks using the Windows `VirtualAlloc` API.
   - `gstack_mmap.c`: gstacks using the Posix `mmap` API.
   - `gstack_mmap_mach.c`: included by `gstack_mmap.c` on macOS (using the Mach kernel) which
      implements a Mach exception handler to catch memory faults in a gstack (and handle them)
      before they get to the debugger.
- `util.c`: error messages.
- `asm`: platform specific assembly routines to switch efficiently between stacks:
   - `asm/longjmp_amd64_win.asm`: for Windows amd64/x84_64.
   - `asm/longjmp_amd64.S`: the System-V ABI (Linux, macOS, etc).
   - `asm/longjmp_arm64.S`: the Aarch64 ABI (ARM64).