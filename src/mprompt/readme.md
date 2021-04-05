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
   - `asm/longjmp_amd64.S`: the AMD64 System-V ABI (Linux, macOS, etc).
   - `asm/longjmp_arm64.S`: the Aarch64 ABI (ARM64).


# Gpools

On systems without overcommit (like macOS, or Linux with overcommit disabled)
or on Windows using exponential commit for pages on the stack,
we need to use our own page fault handler to commit stack pages on-demand.
To detect reliably whether a page fault occurred in one of our stacks and 
also to limit expansion of stacks beyond their maximum size, we reserve
large virtual memory areas, called a `gpool`, where the  `gstack`s are located.

The gpools are linked together with each gpool using by default about 256 GiB virtual
adress space (containing about 32000 8MiB virtual gstacks).
This allows the page fault handler to quickly determine if a fault is in
one our stacks. In between each stack is a _noaccess_ gap (for buffer overflow mitigation) 
and the first stack is used for the gpool info:

```ioke
|--------------------------------------------------------------------  ---------------------|
| mp_gpool_t .... |xxx| gstack 1  .... |xxx| gstack 2 .... |xxx| ...     | gstack N ... |xxx|
|--------------------------------------------------------------------  ---------------------|
  ^                 ^
  meta-data        gap  
```

Other advantanges of using gpools include 
- The stack memory can be grown by doubling (up to 1MiB)
   which can have a performance advantage. 
- Since the memory stays in-process, reusing
   gstacks can be more efficient than going through the OS to unmap/remap memory as
   that needs to be re-zero'd at allocation time.

By default, gpools are only used when overcommit is not available; this means
that by default a gpool is not used on desktop Linux or Windows. You can configure
this though using [`mp_init(config)`](../../include/mprompt.h#L65) where:
- [`config.gpool_enable`](../../test/main.c#L28) can be set to `true` to always enable gpools.
- `config.gpool_max_size` can be set to the initial virtual size of a gpool (256 GiB by default).


# Low-level Layout of Gstacks

## Windows

On Windows, a gstack is allocated as:

```ioke
|------------|
| xxxxxxxxxx | <-- noaccess gap (64 KiB by default)
|------------| <-- base
| committed  |
| ...        | <-- sp
|------------|
| guard page |
|------------|
| reserved   | (committed on demand)
| ...        |
.            .
.            .
|------------| <-- limit 
| xxxxxxxxxx | 
|------------| <-- 8MiB by default
```
The guard page at the end of the committed area will
move down into the reserved area to commit further stack pages on-demand. 
If enabling gpools ([`config.gpool_enable`](test/main.c#L28)), the layout 
of the stack is the same and still uses  guard pages.

## Linux and macOS

On `mmap` based systems the layout depends whether gpools
are enabled. If gpools are enabled (which is automatic of the
OS has no overcommit), the layout is:

```ioke
|------------|
| xxxxxxxxxx |
|------------| <-- base
| committed  |
| ...        | <-- sp
|------------|
| reserved   | (committed on demand)
|            |
.            .
.            .
|------------| <-- limit
| xxxxxxxxxx |
|------------|
```

The `reserved` space is committed on-demand using a signal
handler where the gpool allows the handler to determine
reliably whether a stack can be grown safely (up to the `limit`). 
(As described earlier, this also allows a stack in a gpool to 
grow through doubling which can be more performant, as well as 
allow better reuse of allocated stack memory.)

If the OS has overcommit (and gpools are not enabled explicitly),
the gstack is allocated instead as fully committed from the start
(with read/write access):


```ioke
|------------|
| xxxxxxxxxx |
|------------| <-- base
| committed  |
| ...        | <-- sp
|            |
|            |
.            .
.            .
|------------| <-- limit
| xxxxxxxxxx |
|------------|
```

This is simpler than gpools, as no signal handler is required.
However it will count 8MiB for each stack against the virtual commit count,
even though the actual physical pages are only committed on-demand
by the OS. This may lead to trouble if 
the [overcommit limit](https://www.kernel.org/doc/Documentation/vm/overcommit-accounting) 
is set too low.

   
