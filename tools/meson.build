fs = import('fs')

tools_source = [
  'lblk.c',
  'nvmec.c',
  'xdd.c',
  'xnvme.c',
  'xnvme_file.c',
  'zoned.c',
]

foreach tool_source : tools_source
  executable(
    fs.stem(tool_source),
    tool_source,
    include_directories: xnvme_inc,
    link_with: xnvmelib_static,
    install: true,
  )
endforeach
