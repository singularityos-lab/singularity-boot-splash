# singularity-boot-splash

A toolkit-less KMS boot splash (a Plymouth replacement) for Sinty OS. It
owns the DRM device directly, draws an animated logo and loading bar with Cairo
via the shared `singularity-loginui` renderer, and hands the display off to the
session compositor.

## Build

```sh
git clone --recurse-submodules <url>
meson setup build
ninja -C build
```

## Use of Generative AI

Maintainers may use generative AI tools as assistants while working on singularity-boot-splash. Non-trivial assisted commits disclose the tool, model, and scope of the work.

AI tools may assist with code comments, documentation, repetitive code, and issue triage. Maintainers make project decisions and review every assisted change before it is merged.

Use these trailers for non-trivial assisted commits:

```plain
Assisted-by: <tool>:<model-version>
AI-Scope: <what the tool generated and the prompt or a short prompt summary>
```

Single-line completions, renames, and formatting changes do not need trailers.

Coding agents must also follow [AGENTS.md](AGENTS.md) before changing files,
creating commits, or opening pull requests.
