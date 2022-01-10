xnvmescript_driver = custom_target(
  input: 'xnvme-driver.sh',
  output: 'xnvme-driver',
  command: [find_program('cp'), '@INPUT@', '@OUTPUT@'],
  install: not is_windows,
  install_dir: get_option('bindir')
)
