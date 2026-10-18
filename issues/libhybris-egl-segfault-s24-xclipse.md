# libhybris EGL segfaults on Galaxy S24 (Exynos 2400 / Xclipse)

SM-S921N, Android 17, `ro.hardware.egl=samsung`, platform `erd9945`.
Arch Linux ARM dev install, GUI stack first installed 2026-10-19
(mesa 26.2.5, libglvnd 1.7.0, wayland 1.26.0, firefox 158).

With `--graphics libhybris`, every EGL client dies with SIGSEGV before
printing anything: `eglinfo -B`, firefox, gtk3/gtk4 demos, supertuxkart,
weston-simple-egl, vkcube. `HYBRIS_LOGGING_LEVEL=debug` adds no output.
`libhybris::test_vulkaninfo_loads_android_driver` passes; the other
libhybris integration tests fail ("no GLES renderer reported", "crashed
before rendering"). The same apps reach first paint with `--graphics
cpu` (`apps::` integration tests 4/4).

Not the tawcroot runtime: identical with `TAWCROOT_FASTPATH=0` and with
a 2026-10-18 runtime. `/usr/lib/hybris/` and
`/usr/share/glvnd/egl_vendor.d/00_libhybris.json` are in place.

The S24 is the first Samsung/Xclipse GPU tried (notes/gpu-strategy.md
lists Adreno, Mali and PowerVR). Next step: a backtrace (gdb in the
rootfs, or `debuggerd`-style tombstone) to see whether it dies in the
hybris linker, gralloc/mapper HAL, or Samsung's EGL.
