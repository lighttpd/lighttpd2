modules = {
  'access': {
    'sources': ['mod_access.c'],
  },
  'accesslog': {
    'sources': ['mod_accesslog.c'],
  },
  'auth': {
    'sources': ['mod_auth.c'],
  },
  'balance': {
    'sources': ['mod_balance.c'],
  },
  'cache_disk_etag': {
    'sources': ['mod_cache_disk_etag.c'],
  },
  'debug': {
    'sources': ['mod_debug.c'],
  },
  'deflate': {
    'sources': ['mod_deflate.c'],
    'dependencies': dep_deflate,
  },
  'dirlist': {
    'sources': ['mod_dirlist.c'],
  },
  'expire': {
    'sources': ['mod_expire.c'],
  },
  'fastcgi': {
    'sources': ['mod_fastcgi.c', 'fastcgi_stream.c'],
  },
  'flv': {
    'sources': ['mod_flv.c'],
  },
  'fortune': {
    'sources': ['mod_fortune.c'],
  },
  'gnutls': {
    'sources': ['mod_gnutls.c', 'gnutls_filter.c', 'gnutls_ocsp.c'],
    'dependencies': [dep_gnutls, opt_dep_idn],
  },
  'limit': {
    'sources': ['mod_limit.c'],
  },
  'lua': {
    'sources': ['mod_lua.c'],
    'dependencies': dep_lua,
  },
  'memcached': {
    'sources': ['mod_memcached.c'],
    'dependencies': opt_dep_lua,
  },
  'openssl': {
    'sources': ['mod_openssl.c', 'openssl_filter.c'],
    'dependencies': [dep_openssl, opt_dep_idn],
  },
  'progress': {
    'sources': ['mod_progress.c'],
  },
  'proxy': {
    'sources': ['mod_proxy.c'],
  },
  'redirect': {
    'sources': ['mod_redirect.c'],
  },
  'rewrite': {
    'sources': ['mod_rewrite.c'],
  },
  'scgi': {
    'sources': ['mod_scgi.c'],
  },
  'status': {
    'sources': ['mod_status.c'],
  },
  'throttle': {
    'sources': ['mod_throttle.c'],
  },
  'userdir': {
    'sources': ['mod_userdir.c'],
  },
  'vhost': {
    'sources': ['mod_vhost.c'],
  },
}

modules_build_dir = meson.current_build_dir()

all_modules = []
enabled_modules = []

foreach name, def: modules
  deps = main_deps + def.get('dependencies', [])
  expect_mod = true
  foreach dep: deps
    if not dep.found()
      expect_mod = false
    endif
  endforeach
  mod = shared_module(
    'mod_' + name,
    def['sources'],
    dependencies: main_deps + def.get('dependencies', []),
    c_args: def.get('c_args', []),
    include_directories: inc_dir,
    install: true,
    install_dir: modules_dir,
  )
  all_modules += mod
  if expect_mod
    enabled_modules += mod
  endif
endforeach
