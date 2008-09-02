#! /usr/bin/env python
# encoding: utf-8

import Options, types, sys, Runner
from time import gmtime, strftime, timezone

# the following two variables are used by the target "waf dist"
VERSION='2.0-pre'
APPNAME='lighttpd'

# these variables are mandatory ('/' are converted automatically)
srcdir = '.'
blddir = 'build'

def set_options(opt):
	# the gcc module provides a --debug-level option

	opt.tool_options('compiler_cc')
	opt.tool_options('ragel', tdir = '.')

	#opt.add_option('--with-xattr', action='store_true', help='xattr-support for the stat-cache [default: off]', dest='xattr', default = False)
	#opt.add_option('--with-mysql', action='store_true', help='with mysql-support for the mod_sql_vhost [default: off]', dest = 'mysql', default = False)
	#opt.add_option('--with-postgresql', action='store_true', help='with postgress-support for the mod_sql_vhost [default: off]', dest = 'postgresql', default = False)
	opt.add_option('--with-openssl', action='store_true', help='with openssl-support [default: off]', dest = 'openssl', default = False)
	#opt.add_option('--with-pcre', action='store_true', help='with regex support [default: on]', dest = 'pcre', default = True)
	#opt.add_option('--with-webdav-props', action='store_true', help='with property-support for mod_webdav [default: off]', dest = 'webdavprops', default = False)
	#opt.add_option('--with-sqlite3', action='store_true', help='with property-support [sqlite3] for mod_webdav [default: off]', dest = 'sqlite3', default = False)
	#opt.add_option('--with-bzip', action='store_true', help='with bzip2-support for mod_compress [default: off]', dest = 'bzip', default = False)
	#opt.add_option('--with-zlib', action='store_true', help='with deflate-support for mod_compress [default: on]', dest = 'zlib', default = True)
	#opt.add_option('--with-ldap', action='store_true', help='with LDAP-support for the mod_auth [default: off]', dest = 'ldap', default = False)
	#opt.add_option('--with-libaio', action='store_true', help='with libaio for linux [default: off]', dest = 'libaio', default = False)
	#opt.add_option('--with-libfcgi', action='store_true', help='with libfcgi for fcgi-stat-accel [default: off]', dest = 'libfcgi', default = False)
	opt.add_option('--with-lua', action='store_true', help='with lua 5.1 for mod_magnet [default: off]', dest = 'lua', default = False)
#	opt.add_option('--with-glib', action='store_true', help='with glib support for internal caches [default: on]', dest = 'glib', default = True)
	opt.add_option('--with-all', action='store_true', help='Enable all features', dest = 'all', default = False)
	opt.add_option('--build-static', action='store_true', help='build a static lighttpd with all modules added', dest = 'buildstatic', default = False)
	opt.add_option('--append', action='store', help='Append string to binary names / library dir', dest = 'append', default = '')
	opt.add_option('--lib-dir', action='store', help='Module directory [default: prefix + /lib/lighttpd + append]', dest = 'libdir', default = '')
	
	opt.add_option('--debug', action='store_true', help='Do not compile with -O2', dest = 'debug', default = False)

from Tools.config_c import enumerator_base, check_data

class typesize_enumerator(enumerator_base):
	def __init__(self, conf):
		enumerator_base.__init__(self, conf)
		self.typename = ''

		self.headers       = []

		self.include_paths = []
		self.libs          = []
		self.lib_paths     = []

		self.flags = ''
		self.define = ''
		self.uselib = ''

	def error(self):
		errmsg = 'test program would not run'
		if self.message: errmsg += '\n%s' % self.message
		fatal(errmsg)

	def run_cache(self, retval):
		if self.want_message:
			self.conf.check_message('type size (cached)', '', 1, option=retval['result'])

	def validate(self):
		if not self.typename:
			fatal('type size enumerator needs code to compile and run!')

	def run_test(self):
		code = ''

		code = []
		code.append('#include <stdio.h>\n')
		for header in self.headers:
			code.append('#include <%s>\n' % header)

		code.append('int main(){printf("%%zu\\n", sizeof(%s));\nreturn 0;\n}\n' % self.typename)

		oldlibpath = self.env['LIBPATH']
		oldlib = self.env['LIB']

		self.env['LIB'] = self.libs
		self.env['LIBPATH'] = self.lib_paths

		obj = check_data()
		obj.code     = "\n".join(code)
		obj.env      = self.env
		obj.uselib   = 'lighty ' + self.uselib
		obj.includes = self.include_paths
		obj.force_compiler = 1 # getattr(self, 'force_compiler', None)
		obj.execute  = 1

		ret = self.conf.run_check(obj)
		tsize = ''
		if ret != False: tsize = ret['result']
		self.conf.check_message('typesize %s' % self.typename, '', ret != False, option=' (%s)' % tsize)

		if ret != False:
			self.conf.define(self.define, int(tsize))
		else:
			self.conf.undefine(self.define)

		self.env['LIB'] = oldlib
		self.env['LIBPATH'] = oldlibpath

		return ret

def tolist(x):
	if type(x) is types.ListType:
		return x
	return [x]

def env_mod(conf, use):
	types = [ 'LIB', 'STATICLIB', 'LIBPATH', 'CPPPATH', 'CXXDEFINES', 'CCFLAGS', 'CXXFLAGS', 'LINKFLAGS' ]
	bak = {}
	for t in types:
		bak[t] = conf.env[t]
		for u in use:
			conf.env[t] = tolist(conf.env[t]) + tolist(conf.env['%s_%s' % (t, u)])
	return bak

def env_mod_revert(conf, bak):
	for (k,v) in bak.items():
		conf.env[k] = v

def CHECK_INCLUDE_FILES(conf, header, define, uselib = '', path = None, mandatory = 0, use = []):
	envbak = env_mod(conf, use)
	hconf = conf.create_header_configurator()
	hconf.mandatory = mandatory
	hconf.name = header
	hconf.uselib_store = uselib
	hconf.define = define
	if path: hconf.path += path
	res = hconf.run()
	env_mod_revert(conf, envbak)
	return res

def CHECK_FUNCTION_EXISTS(conf, func, define, headers = None, libs = None, use = []):
	envbak = env_mod(conf, use)
	hconf = conf.create_function_enumerator()
	hconf.function = func
	hconf.define = define
	if headers: hconf.headers += headers
	if libs: hconf.libs += libs
	hconf.custom_code = 'void %s(); void *p;\np=(void*)(%s);' % (func, func)
	res = hconf.run()
	env_mod_revert(conf, envbak)
	return res

def CHECK_TYPE_SIZE(conf, typename, define, headers = None, use = []):
	envbak = env_mod(conf, use)
	hconf = typesize_enumerator(conf)
	hconf.typename = typename
	hconf.define = define
	hconf.headers = []
	if headers: hconf.headers += headers
	res = hconf.run()
	env_mod_revert(conf, envbak)
	return res

def CHECK_LIBRARY_EXISTS(conf, lib, func, define, mandatory = 1, uselib = None, use = []):
	envbak = env_mod(conf, use)
	if not uselib: uselib = lib.upper()
	hconf = conf.create_library_configurator()
	hconf.mandatory = mandatory
	hconf.name = lib
	hconf.uselib_store = uselib
	hconf.define = define
	hconf.code = 'int main() {\nvoid %s(); void *p;\np=(void*)(%s);\nreturn 0;\n}\n' % (func, func)
	res = hconf.run()
	env_mod_revert(conf, envbak)
	return res

def CONFIGTOOL(conf, binary, uselib, define = ''):
	hconf = conf.create_cfgtool_configurator()
	hconf.binary = binary
	hconf.uselib_store = uselib
	hconf.define = define
	res = hconf.run()
	return res

def PKGCONFIG(conf, name, uselib = None, define = '', version = '', mandatory = 0):
	if not uselib: uselib = name
	hconf = conf.create_pkgconfig_configurator()
	hconf.name = name
	hconf.version = version
	hconf.uselib_store = uselib
	hconf.define = define
	hconf.mandatory = mandatory
	res = hconf.run()
	return res

def configure(conf):
	opts = Options.options

	conf.check_tool('compiler_cc')
	conf.check_tool('ragel', tooldir = '.')

	if not opts.libdir:
		opts.libdir = opts.prefix + "/lib/lighttpd" + opts.append
	conf.sub_config('src')

	if opts.all:
#		for o in "xattr mysql postgresql openssl pcre webdavprops sqlite3 bzip zlib ldap libfcgi lua glib".split(" "):
		for o in "lua".split(" "):
			setattr(opts, o, True)

	#if opts.webdavprops:
		#opts.sqlite3 = True
		#opts.xml = opts.uuid = True
	#else:
		#opts.xml = opts.uuid = False

	conf.define("LIGHTTPD_VERSION_ID", 20000)
	conf.define("PACKAGE_NAME", APPNAME)
	conf.define("PACKAGE_VERSION", VERSION)
	conf.define("PACKAGE_BUILD_DATE", strftime("%b %d %Y %H:%M:%S UTC", gmtime()));
	conf.define("LIBRARY_DIR", opts.libdir)

	common_ccflags = [
		'-std=gnu99', '-Wall', '-g', '-Wshadow', '-W', '-pedantic',
		]
	if not opts.debug:
		common_ccflags += [ '-O2' ]
	lighty_common_ccflags = [
		'-fPIC',
		'-DHAVE_CONFIG_H', '-D_GNU_SOURCE',
		'-D_FILE_OFFSET_BITS=64', '-D_LARGEFILE_SOURCE', '-D_LARGE_FILES',
		'-fno-strict-aliasing',
		]
	conf.env['CCFLAGS'] = tolist(conf.env['CCFLAGS']) + common_ccflags
	conf.env['CCFLAGS_lighty'] += lighty_common_ccflags + [ '-DLI_DECLARE_EXPORTS' ]
	conf.env['CCFLAGS_lightymod'] += lighty_common_ccflags
	conf.env['plugin_PREFIX'] = ''
	conf.env['LINKFLAGS_lighty'] += [ '-export-dynamic' ]
	conf.env['LINKFLAGS_lightymod'] += [ '-module', '-export-dynamic', '-avoid-version', '-W,l-no-undefined' ]
	conf.env['LINKFLAGS_thread'] += [ '-pthread' ]

	if opts.buildstatic:
		conf.env['FULLSTATIC'] = True
		conf.env['LINKFLAGS_lighty'] = [ '-static' ]
		conf.env['LIB_lightylast'] += ['m', 'dl']

	if sys.platform == 'linux':
		conf.env['LIB_lighty'] += ['rt']

	CHECK_LIBRARY_EXISTS(conf, "ev", "ev_loop", "HAVE_LIBEV", uselib = 'ev')

	CHECK_INCLUDE_FILES(conf, "sys/devpoll.h", "HAVE_SYS_DEVPOLL_H")
	CHECK_INCLUDE_FILES(conf, "sys/epoll.h", "HAVE_SYS_EPOLL_H")
	CHECK_INCLUDE_FILES(conf, "sys/event.h", "HAVE_SYS_EVENT_H")
	CHECK_INCLUDE_FILES(conf, "sys/mman.h", "HAVE_SYS_MMAN_H")
	CHECK_INCLUDE_FILES(conf, "sys/poll.h", "HAVE_SYS_POLL_H")
	CHECK_INCLUDE_FILES(conf, "sys/port.h", "HAVE_SYS_PORT_H")
	CHECK_INCLUDE_FILES(conf, "sys/prctl.h", "HAVE_SYS_PRCTL_H")
	CHECK_INCLUDE_FILES(conf, "sys/resource.h", "HAVE_SYS_RESOURCE_H")
	CHECK_INCLUDE_FILES(conf, "sys/sendfile.h", "HAVE_SYS_SENDFILE_H")
	CHECK_INCLUDE_FILES(conf, "sys/select.h", "HAVE_SYS_SELECT_H")
	CHECK_INCLUDE_FILES(conf, "sys/syslimits.h", "HAVE_SYS_SYSLIMITS_H")
	CHECK_INCLUDE_FILES(conf, "sys/types.h", "HAVE_SYS_TYPES_H")
	CHECK_INCLUDE_FILES(conf, "sys/uio.h", "HAVE_SYS_UIO_H")
	CHECK_INCLUDE_FILES(conf, "sys/un.h", "HAVE_SYS_UN_H")
	CHECK_INCLUDE_FILES(conf, "sys/wait.h", "HAVE_SYS_WAIT_H")
	CHECK_INCLUDE_FILES(conf, "sys/time.h", "HAVE_SYS_TIME_H")
	CHECK_INCLUDE_FILES(conf, "time.h", "HAVE_TIME_H")
	CHECK_INCLUDE_FILES(conf, "unistd.h", "HAVE_UNISTD_H")
	CHECK_INCLUDE_FILES(conf, "pthread.h", "HAVE_PTHREAD_H")

	CHECK_INCLUDE_FILES(conf, "getopt.h", "HAVE_GETOPT_H")
	CHECK_INCLUDE_FILES(conf, "inttypes.h", "HAVE_INTTYPES_H")

	CHECK_INCLUDE_FILES(conf, "poll.h", "HAVE_POLL_H")
	CHECK_INCLUDE_FILES(conf, "pwd.h", "HAVE_PWD_H")

	CHECK_INCLUDE_FILES(conf, "stddef.h", "HAVE_STDDEF_H")
	CHECK_INCLUDE_FILES(conf, "stdint.h", "HAVE_STDINT_H")
	CHECK_INCLUDE_FILES(conf, "syslog.h", "HAVE_SYSLOG_H")

	CHECK_INCLUDE_FILES(conf, "aio.h", "HAVE_AIO_H")

	CHECK_INCLUDE_FILES(conf, "sys/inotify.h", "HAVE_SYS_INOTIFY_H")
	if conf.is_defined("HAVE_SYS_INOTIFY_H"):
		CHECK_FUNCTION_EXISTS(conf, "inotify_init", "HAVE_INOTIFY_INIT")

	headers = [];
	if conf.is_defined("HAVE_SYS_TYPES_H"): headers.append('sys/types.h')
	if conf.is_defined("HAVE_STDINT_H"): headers.append('stdint.h')
	if conf.is_defined("HAVE_STDDEF_H"): headers.append('stddef.h')
	if conf.is_defined("HAVE_INTTYPES_H"): headers.append('inttypes.h')

	CHECK_TYPE_SIZE(conf, "socklen_t", "HAVE_SOCKLEN_T", headers + ['sys/socket.h'], ['lighty'])

	CHECK_TYPE_SIZE(conf, "long", "SIZEOF_LONG", headers)
	CHECK_TYPE_SIZE(conf, "off_t", "SIZEOF_OFF_T", headers)

	CHECK_FUNCTION_EXISTS(conf, "chroot", "HAVE_CHROOT")
	CHECK_FUNCTION_EXISTS(conf, "crypt", "HAVE_CRYPT")
	CHECK_FUNCTION_EXISTS(conf, "epoll_ctl", "HAVE_EPOLL_CTL")
	CHECK_FUNCTION_EXISTS(conf, "fork", "HAVE_FORK")
	CHECK_FUNCTION_EXISTS(conf, "getrlimit", "HAVE_GETRLIMIT")
	CHECK_FUNCTION_EXISTS(conf, "getuid", "HAVE_GETUID")
	CHECK_FUNCTION_EXISTS(conf, "gmtime_r", "HAVE_GMTIME_R")
	CHECK_FUNCTION_EXISTS(conf, "inet_ntop", "HAVE_INET_NTOP")
	CHECK_FUNCTION_EXISTS(conf, "kqueue", "HAVE_KQUEUE")
	CHECK_FUNCTION_EXISTS(conf, "localtime_r", "HAVE_LOCALTIME_R")
	CHECK_FUNCTION_EXISTS(conf, "lstat", "HAVE_LSTAT")
	CHECK_FUNCTION_EXISTS(conf, "madvise", "HAVE_MADVISE")
	CHECK_FUNCTION_EXISTS(conf, "memcpy", "HAVE_MEMCPY")
	CHECK_FUNCTION_EXISTS(conf, "memset", "HAVE_MEMSET")
	CHECK_FUNCTION_EXISTS(conf, "mmap", "HAVE_MMAP")
	CHECK_FUNCTION_EXISTS(conf, "pathconf", "HAVE_PATHCONF")
	CHECK_FUNCTION_EXISTS(conf, "poll", "HAVE_POLL")
	CHECK_FUNCTION_EXISTS(conf, "port_create", "HAVE_PORT_CREATE")
	CHECK_FUNCTION_EXISTS(conf, "prctl", "HAVE_PRCTL")
	CHECK_FUNCTION_EXISTS(conf, "pread", "HAVE_PREAD")
	CHECK_FUNCTION_EXISTS(conf, "posix_fadvise", "HAVE_POSIX_FADVISE")
	CHECK_FUNCTION_EXISTS(conf, "select", "HAVE_SELECT")
	CHECK_FUNCTION_EXISTS(conf, "sendfile", "HAVE_SENDFILE")
	CHECK_FUNCTION_EXISTS(conf, "sendfile64", "HAVE_SENDFILE64")
	CHECK_FUNCTION_EXISTS(conf, "sendfilev", "HAVE_SENDFILEV")
	CHECK_FUNCTION_EXISTS(conf, "sigaction", "HAVE_SIGACTION")
	CHECK_FUNCTION_EXISTS(conf, "signal", "HAVE_SIGNAL")
	CHECK_FUNCTION_EXISTS(conf, "sigtimedwait", "HAVE_SIGTIMEDWAIT")
	CHECK_FUNCTION_EXISTS(conf, "strptime", "HAVE_STRPTIME")
	CHECK_FUNCTION_EXISTS(conf, "syslog", "HAVE_SYSLOG")
	CHECK_FUNCTION_EXISTS(conf, "writev", "HAVE_WRITEV")
	CHECK_FUNCTION_EXISTS(conf, "inet_aton", "HAVE_INET_ATON")
	CHECK_FUNCTION_EXISTS(conf, "inet_atop", "HAVE_IPV6")
	CHECK_FUNCTION_EXISTS(conf, "strtoll", "HAVE_STRTOLL")

	CHECK_INCLUDE_FILES(conf, "tap.h", "HAVE_LIBTAP_H", uselib = 'tap')
	CHECK_LIBRARY_EXISTS(conf, "tap", "plan_tests", "HAVE_LIBTAP", uselib = 'tap', mandatory = 0)

	#if opts.xattr:
		#CHECK_INCLUDE_FILES(conf, "attr/attributes.h", "HAVE_ATTR_ATTRIBUTES_H")

	#if opts.mysql:
		#if not CONFIGTOOL(conf, 'mysql_config', uselib = 'mysql'):
			#CHECK_INCLUDE_FILES(conf, "mysql.h", "HAVE_MYSQL_H", uselib = 'mysql', path = ['/usr/include/mysql'], mandatory = 1)
			#CHECK_LIBRARY_EXISTS(conf, "mysqlclient", "mysql_real_connect", "HAVE_LIBMYSQL", uselib = 'mysql')
		#else:
			#conf.define("HAVE_MYSQL_H", 1)
			#conf.define("HAVE_LIBMYSQL", 1)

	#if opts.postgresql:
		#if not CONFIGTOOL(conf, 'pg_config', uselib = 'postgresql'):
			#CHECK_INCLUDE_FILES(conf, "libpq-fe.h", "HAVE_LIBPQ_FE_H", uselib = 'postgresql', path = ['/usr/include/pgsql'], mandatory = 1)
			#CHECK_LIBRARY_EXISTS(conf, "pq", "PQconnectdb", "HAVE_LIBPQ", uselib = 'postgresql')
		#else:
			#conf.define("HAVE_LIBPQ_FE_H", 1)
			#conf.define("HAVE_LIBPQ", 1)

	if opts.openssl:
		CHECK_INCLUDE_FILES(conf, "openssl/ssl.h", "HAVE_OPENSSL_SSL_H", uselib = 'openssl', mandatory = 1)
		CHECK_LIBRARY_EXISTS(conf, "crypto", "BIO_f_base64", "HAVE_LIBCRYPTO", uselib = 'openssl')
		CHECK_LIBRARY_EXISTS(conf, "ssl", "SSL_new", "HAVE_LIBSSL", uselib = 'openssl')
		conf.define("OPENSSL_NO_KRB5", 1)

	#if opts.bzip:
		#CHECK_INCLUDE_FILES(conf, "bzlib.h", "HAVE_BZLIB_H", uselib = 'bzip', mandatory = 1)
		#CHECK_LIBRARY_EXISTS(conf, "bz2", "BZ2_bzCompressInit", "HAVE_LIBBZ2", uselib = 'bzip')

	#if opts.ldap:
		#CHECK_INCLUDE_FILES(conf, "ldap.h", "HAVE_LDAP_H", uselib = 'ldap', mandatory = 1)
		#CHECK_LIBRARY_EXISTS(conf, "ldap", "ldap_open", "HAVE_LIBLDAP", uselib = 'ldap')

	#if opts.libaio:
		#CHECK_INCLUDE_FILES(conf, "libaio.h", "HAVE_LIBAIO_H", uselib = 'libaio', mandatory = 1)
		#CHECK_LIBRARY_EXISTS(conf, "aio", "io_getevents", "HAVE_LIBAIO", uselib = 'libaio')

	#if opts.xml:
		#if CONFIGTOOL(conf, 'xml2-config', uselib = 'xml'):
			#CHECK_INCLUDE_FILES(conf, "libxml/tree.h", "HAVE_LIBXML_H", mandatory = 1, uselib='xml', use = ['xml'])
		#else:
			#CHECK_INCLUDE_FILES(conf, "libxml.h", "HAVE_LIBXML_H", mandatory = 1, uselib='xml', use = ['xml'])
		#CHECK_LIBRARY_EXISTS(conf, "xml2", "xmlParseChunk", "HAVE_LIBXML", uselib='xml', use = ['xml'])

	#if opts.pcre:
		#CONFIGTOOL(conf, 'pcre-config', uselib = 'pcre')
		#CHECK_INCLUDE_FILES(conf, "pcre.h", "HAVE_PCRE_H", mandatory = 1, uselib = 'pcre', use = ['pcre'])
		#CHECK_LIBRARY_EXISTS(conf, "pcre", "pcre_exec", "HAVE_LIBPCRE", uselib = 'pcre', use = ['pcre'])

	#if opts.sqlite3:
		#CHECK_INCLUDE_FILES(conf, "sqlite3.h", "HAVE_SQLITE3_H", uselib = 'sqlite3', mandatory = 1)
		#CHECK_LIBRARY_EXISTS(conf, "sqlite3", "sqlite3_reset", "HAVE_SQLITE3", uselib = 'sqlite3')

	PKGCONFIG(conf, "glib-2.0", uselib = 'glib', mandatory = 1)
	incdir = conf.env['CPPPATH_glib'][0]
	conf.env['CPPPATH_glib'] += [ incdir+'/glib-2.0/', incdir + '/glib-2.0/include/' ]
	CHECK_INCLUDE_FILES(conf, "glib.h", "HAVE_GLIB_H", uselib = 'glib', use = ['glib'], mandatory = 1)

	PKGCONFIG(conf, "gthread-2.0", uselib = 'gthread', mandatory = 1)
	incdir = conf.env['CPPPATH_gthread'][0]
	conf.env['CPPPATH_gthread'] += [ incdir+'/glib-2.0/', incdir + '/glib-2.0/include/' ]

	#if opts.libfcgi:
		#CHECK_INCLUDE_FILES(conf, "fastcgi.h", "HAVE_FASTCGI_H", uselib = 'libfcgi')
		#if not conf.is_defined("HAVE_FASTCGI_H"):
			#CHECK_INCLUDE_FILES(conf, "fastcgi/fastcgi.h", "HAVE_FASTCGI_FASTCGI_H", uselib = 'libfcgi', mandatory = 1)
		#CHECK_LIBRARY_EXISTS(conf, "fcgi", "FCGI_Accept", "HAVE_LIBFCGI", uselib = 'libfcgi')

	#if opts.uuid:
		#CHECK_INCLUDE_FILES(conf, "uuid/uuid.h", "HAVE_UUID_H")
		#CHECK_LIBRARY_EXISTS(conf, "uuid", "uuid_generate", "HAVE_LIBUUID")
		#if not conf.is_defined("HAVE_LIBUUID"):
			#CHECK_FUNCTION_EXISTS(conf, "uuid_generate", "HAVE_LIBUUID")

	#if opts.zlib:
		#CHECK_INCLUDE_FILES(conf, "zlib.h", "HAVE_ZLIB_H", uselib = 'zlib', mandatory = 1)
		#if sys.platform != "win32":
			#CHECK_LIBRARY_EXISTS(conf, "z", "deflate", "HAVE_LIBZ", uselib = 'zlib')
		#else:
			#(CHECK_LIBRARY_EXISTS(conf, "z", "deflate", "HAVE_LIBZ", mandatory = 0, uselib = 'zlib') or
				#CHECK_LIBRARY_EXISTS(conf, "zlib", "deflate", "HAVE_LIBZ", mandatory = 0, uselib = 'zlib') or
				#CHECK_LIBRARY_EXISTS(conf, "zdll", "deflate", "HAVE_LIBZ", mandatory = 1, uselib = 'zlib'))

	if opts.lua:
		if not PKGCONFIG(conf, "lua5.1", uselib = 'lua', mandatory = 0):
			conf.env['LIB_lua'] = [ 'm' ]
			CHECK_INCLUDE_FILES(conf, "lua.h", "HAVE_LUA_H", uselib = 'lua', use = ['lua'], mandatory = 1)
			CHECK_LIBRARY_EXISTS(conf, "lua", "lua_setfield", "HAVE_LIBLUA", uselib = 'lua', use = ['lua'])
		else:
			conf.define("HAVE_LUA_H", 1)
			conf.define("HAVE_LIBLUA", 1)

	if not opts.buildstatic:
		CHECK_INCLUDE_FILES(conf, "dlfcn.h", "HAVE_DLFCN_H", uselib = 'dl')
		if conf.is_defined("HAVE_DLFCN_H"):
			CHECK_LIBRARY_EXISTS(conf, "dl", "dlopen", "HAVE_LIBDL", uselib = 'dl')

	CHECK_INCLUDE_FILES(conf, "crypt.h", "HAVE_CRYPT_H", uselib = 'crypt')
	if conf.is_defined("HAVE_CRYPT_H"):
		CHECK_LIBRARY_EXISTS(conf, "crypt", "crypt", "HAVE_LIBCRYPT", uselib = 'crypt')

	conf.write_config_header('src/config.h')

def build(bld):
	# process subfolders from here
	bld.add_subdirs('src')

	#bld.add_manual_dependency('src/main.c', 'some dependency string')

	# the following example shows how to add a dependency on the output of a function
	#def dep_func():
	#	import time
	#	return str(time.time())
	#bld.add_manual_dependency('src/main.c', dep_func)

class TestObject:
	def __init__(self, label, filename):
		self.m_linktask = self
		self.link_task = self
		self.m_outputs = [self]
		self.m_type = 'program'
		self.unit_test = True
		self.env = 0
		self.label = label
		self.filename = filename

	def abspath(self, env):
		return self.filename

	def bldpath(self, env):
		return self.label

def run_tests():
	import UnitTest
	unittest = UnitTest.unit_test()
	unittest.want_to_see_test_output = Options.options.verbose
	unittest.want_to_see_test_error = Options.options.verbose
	unittest.run()
	unittest.print_results()

def shutdown():
	if Options.commands['check']: run_tests()

def dist_hook():
	from os import system
	system('ragel src/config_parser.rl')
	system('ragel src/http_request_parser.rl')
