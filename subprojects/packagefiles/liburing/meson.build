project(
  'liburing',
  'c',
  version: '2.0'
)
fs = import('fs')

if not fs.exists('config-host.h')
  run_command('./configure', capture: false, check: true)
endif
if not fs.exists('src' / 'liburing.a')
  run_command('make', capture: false, check: true)
endif

cc = meson.get_compiler('c')

lib_paths = [
  
]

liburing_inc = include_directories( 'src' / 'include')
liburing_dep = declare_dependency(
  dependencies : cc.find_library(
    'uring',
    dirs: [
      meson.current_source_dir() / 'src',
      meson.current_source_dir() / 'src' / 'include'
    ],
    static: true
  ),
  include_directories: liburing_inc,
  variables: {
    'lib_paths': meson.current_source_dir() / 'src' / 'liburing.a'
  }
)

meson.override_dependency('uring', liburing_dep)
