# fastfetch GPU detection crashes through libhybris

Debian sid fastfetch on Yoro's arm64 phone (Exynos/Xclipse, Android 14,
app `me.phie.tawc.dev`, systemd `--init` session, app shell via
`--join`): every module prints except GPU, which dies with every
detection method. pci finds no PCI devices and falls through to
Vulkan (SIGABRT); opencl/opengl SIGSEGV.

Cause narrowed: the app shell has
`LD_LIBRARY_PATH=/usr/lib/hybris/gl-shims:/usr/lib/hybris`
(`RootfsEnv.kt`), so fastfetch's `dlopen("libvulkan.so.1")` (and its
EGL/GL/OpenCL probes) land on libhybris. With `LD_LIBRARY_PATH=` the GPU
module just reports nothing and exits cleanly.

Open: whether libhybris also aborts this way outside a systemd session
(plain session), and where (libhybris vs vendor driver vs tawcroot).
`<store>/fault.log` holds the SEGV pc for the GL/CL paths.
