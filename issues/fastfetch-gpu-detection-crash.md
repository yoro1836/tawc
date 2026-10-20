# fastfetch GPU detection crashes under tawcroot (arm64 phone)

Debian sid fastfetch on Yoro's arm64 phone (Exynos, Android 14, app
`me.phie.tawc.dev`, tawcroot `--init` session, shell via `--join`):
every other module prints, but the GPU module dies with every detection
method (config `{"modules":[{"type":"gpu","detectionMethod":"<m>"}]}`):

- `pci`, `vulkan`: SIGABRT
- `opengl`, `opencl`: SIGSEGV

Not investigated yet. Start with `<store>/fault.log` (SEGV pc) and
`<store>/eacces.log` around one run; `pci` aborting suggests a shared
`/sys` or `/dev/dri` probe rather than a GL driver.
