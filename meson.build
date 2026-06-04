project('viply', 'c',
  version : '0.1.0',
  default_options : ['warning_level=3', 'c_std=c11'])

vips_dep = dependency('vips')
thread_dep = dependency('threads')

sources = [
  'src/main.c',
  'src/viply.c',
  'src/utils.c'
]

executable('viply',
  sources,
  dependencies : [vips_dep, thread_dep],
  install : true)
