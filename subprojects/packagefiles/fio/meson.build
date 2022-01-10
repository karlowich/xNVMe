project(
  'fio',
  'c',
  version: '3.28',
)
fs = import('fs')

if get_option('build_subprojects') and not fs.exists('config-host.h')
  run_command('./configure', capture: false, check: true)
endif
if get_option('build_subprojects') and not fs.exists('fio')
  run_command('make', capture: false, check: true)
endif

fio_inc = include_directories('.')
fio_dep = declare_dependency(
  include_directories: fio_inc
)

meson.override_dependency('fio', fio_dep)
