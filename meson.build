project(
  'lighttpd',
  'c',
  default_options: [
    'buildtype=debugoptimized',
    'c_std=c99', # gnu99?
  ],
  version: '2.0.0',
  license: 'MIT',
)

libexec_dir = get_option('prefix') / get_option('libexecdir')
# include_dir = get_option('prefix') / get_option('includedir')
modules_dir = get_option('prefix') / get_option('libdir') / (meson.project_name() + '-' + meson.project_version())
lua_dir = get_option('prefix') / get_option('datadir') / 'lighttpd2/lua'

search_includes = include_directories(get_option('search-inc'))
search_libs = get_option('search-lib')

compiler = meson.get_compiler('c')
dep_not_found = dependency('', required: false)

if target_machine.system() == 'windows'
  add_project_arguments(
    '-DNVALGRIND',
    language: 'c'
  )
  # link ws2_32 ?
  # ... ?
endif

# ragel to compile parsers
ragel_bin = find_program('ragel')
ragel_gen = generator(
  ragel_bin,
  output: '@BASENAME@.c',
  arguments: ['-C', '-T1', '-o', '@OUTPUT@', '@INPUT@'],
)
ragel_gen_t0 = generator(
  ragel_bin,
  output: '@BASENAME@.c',
  arguments: ['-C', '-T0', '-o', '@OUTPUT@', '@INPUT@'],
)

add_project_arguments(
  '-D_FILE_OFFSET_BITS=64',
  '-D_LARGEFILE_SOURCE',
  '-D_LARGE_FILES',
  language: 'c',
)

conf_data = configuration_data()
conf_data.set_quoted('PACKAGE_VERSION', meson.project_version())
conf_data.set_quoted('PACKAGE_NAME', meson.project_name())
conf_data.set_quoted('DEFAULT_LIBEXECDIR', libexec_dir)
conf_data.set_quoted('DEFAULT_LIBDIR', modules_dir)
# if target_machine.system() == 'windows'
#   conf_data.set_quoted('DEFAULT_LIBDIR', 'lib')
# endif
conf_data.set_quoted('DEFAULT_LUADIR', lua_dir)

dep_threads = dependency('threads')
dep_gthread = dependency('gthread-2.0', version: '>=2.16')
dep_gmodule = dependency('gmodule-2.0', version: '>=2.16')

# find libev manually
debug('libs:', search_libs)
dep_ev = compiler.find_library(
  'ev',
  has_headers: 'ev.h',
  dirs: search_libs,
  header_include_directories: search_includes,
)
if not compiler.has_function(
  'ev_time',
  prefix: '#include <ev.h>',
  dependencies: dep_ev,
  include_directories: search_includes,
)
  error('Missing ev_time() in libev')
endif

if get_option('lua')
  dep_lua = dependency('lua5.4', 'lua5.3', 'lua5.1', 'lua-5.1', 'lua')
  opt_dep_lua = dep_lua
  conf_data.set10('HAVE_LUA_H', true)
else
  dep_lua = disabler()
  opt_dep_lua = dep_not_found
endif

if get_option('unwind')
  opt_dep_unwind = dependency('libunwind')
  conf_data.set10('HAVE_LIBUNWIND', true)
else
  opt_dep_unwind = dep_not_found
endif

if get_option('openssl')
  dep_openssl = dependency('openssl', version: '>=1.1')  # should find both ssl and crypto
else
  dep_openssl = disabler()
endif

if get_option('gnutls')
  dep_gnutls = dependency('gnutls')
else
  dep_gnutls = disabler()
endif

if get_option('sni')
  opt_dep_idn = declare_dependency(
    compile_args: '-DUSE_SNI',
    dependencies: dependency('libidn')
  )
else
  opt_dep_idn = dep_not_found
endif

if get_option('bzip2')
  opt_dep_bzip2 = dependency('bzip2', required: false)
  if not opt_dep_bzip2.found()
    opt_dep_bzip2 = compiler.find_library(
      'bz2',
      has_headers: 'bzlib.h',
      dirs: search_libs,
      header_include_directories: search_includes,
    )
    if not compiler.has_function(
      'BZ2_bzCompressInit',
      prefix: '#include <bzlib.h>',
      dependencies: opt_dep_bzip2,
      include_directories: search_includes,
    )
      error('Found libbz2, but missing BZ2_bzCompressInit()')
    endif
  endif
  conf_data.set10('HAVE_BZIP', true)
else
  opt_dep_bzip2 = dep_not_found
endif

if get_option('deflate') ## zlib/gzip??
  opt_dep_zlib = dependency('zlib')
  conf_data.set10('HAVE_ZLIB', true)
else
  opt_dep_zlib = dep_not_found
endif

if opt_dep_bzip2.found() or opt_dep_zlib.found()
  dep_deflate = declare_dependency(dependencies: [opt_dep_bzip2, opt_dep_zlib])
else
  dep_deflate = disabler()
endif

warn_c_args = [
  '-Wshadow',
  '-W',
  '-pedantic',
]
warn_link_args = []
if get_option('extra-warnings')
  warn_c_args += [
    '-Wmissing-declarations',
    '-Wdeclaration-after-statement',
    '-Wcast-align',
    '-Wsign-compare',
    '-Wnested-externs',
    '-Wpointer-arith',
    '-Wmissing-prototypes',
    '-Wshadow',
    '-Wno-pointer-sign',
    '-Wformat-security',
  ]
endif

check_sys_includes = [
  'inttypes.h',
  'stddef.h',
  'stdint.h',
  'sys/mman.h',
  'sys/resource.h',
  'sys/sendfile.h',
  'sys/types.h',
  'sys/uio.h',
  'sys/un.h',
  'unistd.h',
]

if get_option('profiler')
  check_sys_includes += ['execinfo.h']
  add_project_arguments(
    '-DWITH_PROFILER',
    language: 'c'
  )
endif

check_libc_functions = [
  'getrlimit',
  'gmtime_r',
  'inet_aton',
  'inet_ntop',
  'localtime_r',
  'madvise',
  'mmap',
  'posix_fadvise',
  'sendfile',
  'sendfile64',
  'sendfilev',
  'writev',
  'accept4',
]

# run compiler/env checks
foreach sys_include: check_sys_includes
  if compiler.has_header(sys_include)
    conf_data.set10('HAVE_' + sys_include.underscorify().to_upper(), true)
  endif
endforeach
foreach libc_function: check_libc_functions
  if compiler.has_function(libc_function)
    conf_data.set10('HAVE_' + libc_function.underscorify().to_upper(), true)
  endif
endforeach
add_project_arguments(
  compiler.get_supported_arguments(warn_c_args),
  language: 'c'
)
add_project_link_arguments(
  compiler.get_supported_link_arguments(warn_link_args),
  language: 'c'
)

# manual libcrypt with crypt_r check
lib_crypt = compiler.find_library(
  'crypt',
  required: false,
  has_headers: 'crypt.h',
  dirs: search_libs,
  header_include_directories: search_includes,
)
if lib_crypt.found()
  if compiler.has_function(
    'crypt_r',
    prefix: '#include <crypt.h>',
    dependencies: lib_crypt,
    include_directories: search_includes,
  )
    conf_data.set10('HAVE_CRYPT_H', true)
    conf_data.set10('HAVE_CRYPT_R', true)
  else
    error('Found libcrypt, but missing crypt_r')
  endif
else
  warning('Missing libcrypt, using crypt instead of crypt_r')
endif

# need libm for fmod in throttle.c
lib_m = compiler.find_library(
  'm',
  dirs: search_libs,
  header_include_directories: search_includes,
)

# IPv6 support is mandatory by default
if get_option('ipv6')
  if compiler.has_type(
    'struct sockaddr_in6',
    prefix: '\n'.join([
      '#include <sys/types.h>',
      '#include <sys/socket.h>',
      '#include <netinet/in.h>',
    ]),
  )
    conf_data.set10('HAVE_IPV6', true)
  else
    error('Missing struct sockaddr_in6, needed for IPv6 support')
  endif
endif

# auto detect 'struct sockaddr_storage' - should work fine without it.
if compiler.has_type(
  'struct sockaddr_storage',
  prefix: '#include <sys/socket.h>',
)
  conf_data.set10('HAVE_SOCKADDR_STORAGE', true)
endif

if target_machine.system() == 'freebsd'
  lib_kvm = compiler.find_library(
    'kvm',
    has_headers: 'kvm.h',
    dirs: search_libs,
    header_include_directories: search_includes,
  )
  if not compiler.has_function(
    'kvm_open',
    prefix: '#include <kvm.h>',
    dependencies: lib_kvm,
    include_directories: search_includes,
  )
    error('Found libkvm, but missing kvm_open')
  endif
else
  lib_kvm = dep_not_found
endif

main_deps = [
  dep_threads,
  dep_gthread,
  dep_gmodule,
  dep_ev,
]

subdir('contrib')
subdir('doc')
subdir('include')
subdir('src')
subdir('tests')

summary(
  {
    'libexec path': libexec_dir,
    'modules path': modules_dir,
    'lua plugin path': lua_dir,
  },
  section: 'Paths',
)
summary(
  {
    'lua': get_option('lua'),
    'ipv6': get_option('ipv6'),
    'config-parser': get_option('config-parser'),
    'unwind': get_option('unwind'),
    'openssl': get_option('openssl'),
    'gnutls': get_option('gnutls'),
    'sni': get_option('sni'),
    'bzip2': get_option('bzip2'),
    'deflate': get_option('deflate'),
    'profiler': get_option('profiler'),
  },
  section: 'Features',
)
summary(
  {
    'detected libcrypt/crypt_r()': lib_crypt.found(),
    'detected mmap()': conf_data.get('HAVE_MMAP', 0) == 1,
    'detected writev()': conf_data.get('HAVE_WRITEV', 0) == 1,
    'detected sendfile*()': (
      conf_data.get('HAVE_SENDFILE', 0) == 1
      or conf_data.get('HAVE_SENDFILE64', 0) == 1
      or conf_data.get('HAVE_SENDFILEV', 0) == 1
    ),
  },
  section: 'Detected',
)
