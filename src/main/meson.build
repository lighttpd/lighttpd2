src_shared = [
  'angel.c',
  'angel_fake.c',
  'actions.c',
  'base_lua.c',
  'backends.c',
  'chunk.c',
  'chunk_parser.c',
  'collect.c',
  'condition.c',
  'connection.c',
  'environment.c',
  'etag.c',
  'filter.c',
  'filter_chunked.c',
  'filter_buffer_on_disk.c',
  'http_headers.c',
  'lighttpd_glue.c',
  'log.c',
  'mimetype.c',
  'network.c',
  'network_write.c',
  'network_writev.c',
  'network_sendfile.c',
  'options.c',
  'pattern.c',
  'plugin.c',
  'request.c',
  'response.c',
  'server.c',
  'stat_cache.c',
  'stream.c',
  'stream_http_response.c',
  'stream_simple_socket.c',
  'throttle.c',
  'value.c',
  'virtualrequest.c',
  'worker.c',
  'plugin_core.c',
] + ragel_gen.process(
  'http_range_parser.rl',
  'http_request_parser.rl',
  'http_response_parser.rl',
  'url_parser.rl',
)

if not get_option('config-parser')
  conf_data.set10('WITHOUT_CONFIG_PARSER', true)
else
  src_shared += ragel_gen_t0.process('config_parser.rl')
endif

if get_option('lua')
  src_shared += [
    'actions_lua.c',
    'condition_lua.c',
    'config_lua.c',
    'value_lua.c',
    'chunk_lua.c',
    'core_lua.c',
    'environment_lua.c',
    'filters_lua.c',
    'http_headers_lua.c',
    'physical_lua.c',
    'request_lua.c',
    'response_lua.c',
    'stat_lua.c',
    # 'subrequest_lua.c',
    'virtualrequest_lua.c',
  ]
endif

lib_shared = library(
  'lighttpd2-shared-' + meson.project_version(),
  src_shared,
  include_directories: [inc_dir] +  search_includes,
  dependencies: [
    main_deps,
    opt_dep_lua,
    lib_m,  # TODO: fmod in throttle.c
  ],
  link_with: lib_common,
  install: true,
)

bin_worker = executable(
  'lighttpd2-worker',
  'lighttpd_worker.c',
  include_directories: [inc_dir] +  search_includes,
  dependencies: [
    main_deps,
    opt_dep_lua,
  ],
  link_with: [
    lib_shared,
    lib_common,
  ],
  install: true,
  install_dir: libexec_dir,
)
