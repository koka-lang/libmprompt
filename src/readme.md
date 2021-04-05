# Sources

There are two libraries provided:

- `mprompt`: the primitive library that provides 
  multi-prompt control. We view this as an interface the OS (or engine) should
  provide. As a programmer, using this abstration is still a bit low-level though.

- `mpeff`: a small example library that uses `libmprompt` to implement
  efficient algebraic effect handlers. These give more structure and are more
  suitable for direct use in programming language.
