# A so-called pre-compiled-header from the build of fio
shared_library(
  meson.project_name() + '-fio-engine',
  ['xnvme_fioe.c'],
  include_directories: [ xnvme_inc ],
  c_args: [
    '-g',
    '-Wno-unused-parameter',
    '-D_GNU_SOURCE',
    '-include', 'config-host.h',
  ],
  dependencies: [ fio_dep ],
  link_with: xnvmelib_static,
  link_args: '-lrt',
  install: true,
)

install_data('xnvme-compare.fio', install_dir : get_option('datadir') / meson.project_name())
install_data('xnvme-verify.fio', install_dir : get_option('datadir') / meson.project_name())
install_data('xnvme-zoned.fio', install_dir : get_option('datadir') / meson.project_name())
install_data('xnvme-compare.fio', install_dir : get_option('datadir') / meson.project_name())
