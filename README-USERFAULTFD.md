# Outstanding issues with userfaultfd

- When userfaultfd is enabled the kernel doesn't create new pages full of zeros
  when addresses are accessed for the first time. There should be a way to do
  this manually in the userfaultfd handler thread, but the quick fix was just
  to zero the entire loom before we do anything with it. This may be bad for
  memory consumption on devices with really low amounts of memory, and it has
  a slight impact on boot time (outweighed by the increased perf of uffd).
- If a signal is received while the userfaultfd thread is processing (e.g. the
  window resizes and we get a SIGWINCH), the main thread might resume when it
  shouldn't yet, which currently causes a segfault (the bad kind, not the kind
  that's supposed to happen).
- As I did in my `userfaultfd-old-wip` branch, the userfaultfd kernel headers
  should be provided by the `linuxHeaders` Nix package and not vendored in
  `pkg/urbit/include/linux`, for licensing reasons. However, that package is
  huge and nixpkgs would have to be updated as well, as the provided version
  of linuxHeaders is too old.
