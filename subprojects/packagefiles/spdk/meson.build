project(
  'spdk',
  'c',
  version: '21.10',
)
fs = import('fs')

if not fs.exists('build')
  message('Patching ..')
  run_command('./spdk_patches.sh', capture: false, check: true)
  message('Configuring ..')
  run_command('./spdk_configure.sh', capture: false, check: true)
endif
if not fs.exists('build' / 'lib' / 'src' / 'spdk_nvme.a')
  message('Building ..')
  run_command('./spdk_build.sh', capture: false, check: true)
endif

message('Setting up dependency ..')
cc = meson.get_compiler('c')

spdk_inc = include_directories(
  'dpdk' / 'build' / 'include',
  'build' / 'include'
)

spdk_libnames = [
  'spdk_nvme',
  'spdk_env_dpdk',
  'spdk_sock',
  'spdk_sock_posix',
  'spdk_rpc',
  'spdk_trace',
  'spdk_jsonrpc',
  'spdk_json',
  'spdk_util',
  'spdk_log'
]

dpdk_libnames = [
  'rte_eal',
  'rte_telemetry',
  'rte_bus_pci',
  'rte_pci',
  'rte_ring',
  'rte_mempool',
  'rte_kvargs'
]

spdk_paths = []
spdk_libs = []
foreach libname : spdk_libnames + dpdk_libnames
  lib_dep = cc.find_library(
    libname,
    dirs: [
      meson.current_source_dir() / 'build' / 'lib',
      meson.current_source_dir() / 'dpdk' / 'build' / 'lib',
    ],
    static: true
  )
  spdk_libs += lib_dep

  # Create a bunch of paths
  paths = [
    meson.current_source_dir() / 'build' / 'lib' / 'lib' + libname + '.a',
    meson.current_source_dir() / 'dpdk' / 'build' / 'lib' / 'lib' + libname + '.a',
  ]
  foreach path : paths
    if lib_dep.found() and fs.exists(path)
      spdk_paths += path
    endif
  endforeach
endforeach

spdk_link_args = ['-Wl,--whole-archive', '-Wl,--no-as-needed'] + spdk_paths + ['-Wl,--no-whole-archive', '-Wl,--as-needed']

# Construct link_args based on the above
spdk_dep = declare_dependency(
  dependencies: spdk_libs,
  link_args: spdk_link_args,
  include_directories: spdk_inc,
  variables: {'lib_paths': ' '.join(spdk_paths)}
)

meson.override_dependency('spdk', spdk_dep)
