# singularity-boot-splash

A toolkit-less KMS boot splash (a Plymouth replacement) for Singularity OS. It
owns the DRM device directly, draws an animated logo and loading bar with Cairo
via the shared `singularity-loginui` renderer, and hands the display off to the
session compositor.

## Build

```sh
git clone --recurse-submodules <url>
meson setup build
ninja -C build
```
