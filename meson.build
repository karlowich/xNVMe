project(
  'xnvme',
  'c',
  version: '0.0.27',
  license: 'APACHE',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'wrap_mode=forcefallback',
    'buildtype=release',
  ],
  meson_version: '>=0.56'
)
pconf = import('pkgconfig')

#
# Pre-flight check, operating system and compiler arguments
#
system_support = ['freebsd', 'linux', 'windows']
system = host_machine.system()
if not system_support.contains(system)
    error('Unsupported system type "@0@"'.format(exec_env))
endif
foreach sys : system_support
    set_variable('is_' + sys, system == sys)
endforeach
is_nixy = is_linux or is_freebsd

cc = meson.get_compiler('c')
cc_flags = [
  '-Wno-missing-braces',
]
add_project_arguments(
  cc.get_supported_arguments(cc_flags),
  language: 'c'
)

#
# The project itself
#
project_version_major = meson.project_version().split('.')[0]
project_version_minor = meson.project_version().split('.')[1]
project_version_patch = meson.project_version().split('.')[2]

#
# Configuration
#

conf_data = configuration_data()
conf_data.set('XNVME_VERSION_MAJOR', project_version_major)
conf_data.set('XNVME_VERSION_MINOR', project_version_minor)
conf_data.set('XNVME_VERSION_PATCH', project_version_patch)

conf_data.set('XNVME_DEBUG_ENABLED', get_option('buildtype') == 'debug')

conf_data.set('XNVME_BE_WINDOWS_ENABLED', is_windows)

conf_data.set('XNVME_BE_LINUX_ENABLED', is_linux)
conf_data.set('XNVME_BE_LINUX_BLOCK_ENABLED', is_linux)
conf_data.set('XNVME_BE_LINUX_BLOCK_ZONED_ENABLED', is_linux)
conf_data.set('XNVME_BE_LINUX_LIBAIO_ENABLED', is_linux and get_option('with-libaio'))
conf_data.set('XNVME_BE_LINUX_LIBURING_ENABLED', is_linux and get_option('with-liburing'))

conf_data.set('XNVME_BE_FBSD_ENABLED', is_freebsd)
conf_data.set('XNVME_BE_POSIX_ENABLED', is_nixy)

conf_data.set('XNVME_BE_SPDK_ENABLED', is_nixy and get_option('with-spdk'))
conf_data.set('XNVME_BE_SPDK_TRANSPORT_PCIE_ENABLED', is_nixy and get_option('with-spdk'))
conf_data.set('XNVME_BE_SPDK_TRANSPORT_TCP_ENABLED', is_nixy and get_option('with-spdk'))
conf_data.set('XNVME_BE_SPDK_TRANSPORT_RDMA_ENABLED', is_nixy and get_option('with-spdk'))
conf_data.set('XNVME_BE_SPDK_TRANSPORT_FC_ENABLED', is_nixy and get_option('with-spdk'))

conf_data.set('XNVME_BE_ASYNC_ENABLED', true)
conf_data.set('XNVME_BE_ASYNC_EMU_ENABLED', get_option('enable-async-emu'))
conf_data.set('XNVME_BE_ASYNC_THRPOOL_ENABLED', get_option('enable-async-thrpool'))

conf = configure_file(
  configuration : conf_data,
  output : 'xnvme_config.h',
)
conf_inc = include_directories('.')

# Provide configuration via header-file 'xnvme_config.h'
add_global_arguments(['-include', 'xnvme_config.h'], language: 'c')

# Scripts
subdir('scripts')

#
# Third-party dependencies -- this is actually lib-specific... should probably be moved into
# 'src/meson.build'?
#
thread_dep = dependency('threads')

fio_cfg_h = 'subprojects'/'fio'/'config-host.h'
fio_dep = dependency('fio', required: get_option('enable-fio-io-engine'))

rt_dep = cc.find_library('rt')
if not cc.has_function('clock_gettime', prefix: '#include <time.h>', dependencies: rt_dep)
  error('clock_gettime not found')
endif

aio_dep = cc.find_library(
  'aio',
  required: conf_data.get('XNVME_BE_LINUX_LIBAIO_ENABLED'),
  has_headers: ['libaio.h'],
)

uring_dep = dependency('uring', required: is_linux and get_option('with-liburing'))

math_dep = cc.find_library(
  'm',
  required: conf_data.get('XNVME_BE_SPDK_ENABLED'),
  has_headers: ['math.h'],
)
uuid_dep = cc.find_library(
  'uuid',
  required: conf_data.get('XNVME_BE_SPDK_ENABLED'),
  has_headers: ['uuid/uuid.h'],
)
numa_dep = cc.find_library(
  'numa',
  required: is_linux and conf_data.get('XNVME_BE_SPDK_ENABLED'),
  has_headers: ['numaif.h'],
)
execinfo_dep = cc.find_library(
  'execinfo',
  required: is_freebsd and conf_data.get('XNVME_BE_SPDK_ENABLED'),
  has_headers: ['execinfo.h'],
)
elf_dep = cc.find_library(
  'elf',
  required: is_freebsd and conf_data.get('XNVME_BE_SPDK_ENABLED'),
  has_headers: ['sys/elf_common.h'],
)
spdk_dep = dependency('spdk', required: conf_data.get('XNVME_BE_SPDK_ENABLED'))

# Headers
subdir('include')

# Library source
subdir('src')

# Tests, tools, fio io-engine, and code examples
if get_option('enable-tests')
  subdir('tests')
endif
if get_option('enable-tools')
  subdir('tools')
endif
if get_option('enable-fio-io-engine')
  subdir('tools/fio-engine')
endif
if get_option('enable-examples')
  subdir('examples')
endif

# pkg-config -- describing how to consume the xNVMe library
pconf.generate(
  libraries: [xnvmelib_deps_shared] + [xnvmelib_bundle_dep],
  version: meson.project_version(),
  name: 'xNVMe',
  filebase: 'libxnvme',
  description : 'Libraries and tools for traditional and emerging I/O interfaces'
)

# Dist-Package generation: deb, nsis, and tar -- tar can probably just be 'meson dist'
