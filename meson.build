project('singularity-boot-splash', 'c',
  version: '0.1.0',
  meson_version: '>= 0.59.0',
)

drm_dep = dependency('libdrm')

loginui_dep = dependency('singularity-loginui',
  fallback: ['singularity-loginui', 'loginui_dep']
)

executable('singularity-boot-splash',
  ['src/boot_splash_main.c'],
  dependencies: [drm_dep, loginui_dep],
  install: true,
)
