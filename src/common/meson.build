src_common = [
  'angel_connection.c',
  'angel_data.c',
  'buffer.c',
  'encoding.c',
  'events.c',
  'fetch.c',
  'idlist.c',
  'jobqueue.c',
  'memcached.c',
  'mempool.c',
  'module.c',
  'radix.c',
  'sys_memory.c',
  'sys_socket.c',
  'tasklet.c',
  'utils.c',
  'value.c',
  # 'value_impl.c', -- "templated", gets included in main and angel
  'waitqueue.c',
] + ragel_gen.process('ip_parsers.rl')

if get_option('profiler')
  src_common += ['profiler.c']
endif

lib_common = library(
  'lighttpd2-common-' + meson.project_version(),
  src_common,
  include_directories: inc_dir,
  dependencies: [
    main_deps,
    dep_unwind,
    lib_crypt,
    lib_kvm,
  ],
  install: true,
)
