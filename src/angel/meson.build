src_angel_shared = [
  'angel_log.c',
  'angel_plugin.c',
  'angel_plugin_core.c',
  'angel_proc.c',
  'angel_server.c',
  'angel_value.c',
] + ragel_gen.process(
  'angel_config_parser.rl',
)

lib_shared_angel = library(
  'lighttpd2-sharedangel-' + meson.project_version(),
  src_angel_shared,
  include_directories: [inc_dir] +  search_includes,
  dependencies: [
    main_deps,
  ],
  link_with: lib_common,
  install: true,
)

bin_angel = executable(
  'lighttpd2',
  'angel_main.c',
  include_directories: [inc_dir] +  search_includes,
  dependencies: [
    main_deps,
  ],
  link_with: [
    lib_common,
    lib_shared_angel,
  ],
  install: true,
  install_dir: get_option('sbindir')
)
