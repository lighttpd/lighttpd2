#!/usr/bin/env python
# encoding: utf-8

"""
waf build script for Lighttpd 2.x
License and Copyright: see COPYING file
"""

import Options, types, os, sys, Runner, Utils
from time import gmtime, strftime, timezone

LIGHTTPD_VERSION_MAJOR=2
LIGHTTPD_VERSION_MINOR=0
LIGHTTPD_VERSION_PATCH=0

# the following two variables are used by the target "waf dist"
VERSION='%d.%d.%d' % (LIGHTTPD_VERSION_MAJOR,LIGHTTPD_VERSION_MINOR,LIGHTTPD_VERSION_PATCH)
APPNAME='lighttpd'

# these variables are mandatory ('/' are converted automatically)
srcdir = '.'
blddir = 'build'


def set_options(opt):
	opt.tool_options('compiler_cc')
	opt.tool_options('ragel', tdir = '.')
	
	# ./waf configure options
	opt.add_option('--with-lua', action='store_true', help='with lua 5.1 for mod_magnet', dest = 'lua', default = False)
	opt.add_option('--with-openssl', action='store_true', help='with openssl-support [default: off]', dest='openssl', default=False)
	opt.add_option('--with-all', action='store_true', help='Enable all features', dest = 'all', default = False)
	opt.add_option('--static', action='store_true', help='build a static lighttpd with all modules added', dest = 'static', default = False)
	opt.add_option('--append', action='store', help='Append string to binary names / library dir', dest = 'append', default = '')
	opt.add_option('--lib-dir', action='store', help='Module directory [default: prefix + /lib/lighttpd + append]', dest = 'libdir', default = '')
	opt.add_option('--debug', action='store_true', help='Do not compile with -O2', dest = 'debug', default = False)
	opt.add_option('--extra-warnings', action='store_true', help='show more warnings while compiling', dest='extra_warnings', default=False)


def configure(conf):
	opts = Options.options

	#conf.check_message_2('The waf build system has been disabled, please use cmake for the time being.', 'RED')
	#sys.exit(1)

	conf.define('LIGHTTPD_VERSION_MAJOR', LIGHTTPD_VERSION_MAJOR)
	conf.define('LIGHTTPD_VERSION_MINOR', LIGHTTPD_VERSION_MINOR)
	conf.define('LIGHTTPD_VERSION_PATCH', LIGHTTPD_VERSION_PATCH)
	conf.define('PACKAGE_NAME', APPNAME)
	conf.define('PACKAGE_VERSION', VERSION)
	conf.define('PACKAGE_BUILD_DATE', strftime("%b %d %Y %H:%M:%S UTC", gmtime()));
	conf.define('LIBRARY_DIR', opts.libdir)

	conf.check_tool('compiler_cc')
	conf.check_tool('ragel', tooldir = '.')

	conf.env['CCFLAGS'] = tolist(conf.env['CCFLAGS'])
	conf.env['CCFLAGS'] += [
		'-std=gnu99', '-Wall', '-Wshadow', '-W', '-pedantic', '-fPIC',
		'-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64', '-D_LARGEFILE_SOURCE', '-D_LARGE_FILES',
		'-g', '-g2'
#	'-fno-strict-aliasing',
	]

	if sys.platform != 'darwin':
		conf.env['LINKFLAGS'] += [ '-export-dynamic' ]
		conf.env['LINKFLAGS_lighty_mod'] += [ '-module', '-avoid-version', '-W,l-no-undefined' ]
	else:
		# OSX aka darwin needs special treatment
		conf.env['shlib_PATTERN'] = 'lib%s.so'
		conf.env['LINKFLAGS'] += ['-flat_namespace']
		conf.env['LINKFLAGS_lighty_mod'] += ['-undefined', 'dynamic_lookup']

	
	# check for available libraries
	conf.check_cfg(package='glib-2.0', uselib_store='glib', atleast_version='2.16', args='--cflags --libs', mandatory=True)
	conf.check_cfg(package='gmodule-2.0', uselib_store='gmodule', atleast_version='2.16', args='--cflags --libs', mandatory=True)
	conf.check_cfg(package='gthread-2.0', uselib_store='gthread', atleast_version='2.16', args='--cflags --libs', mandatory=True)
	conf.check(header_name="glib.h", uselib='glib', mandatory=True)
	glib_version = conf.check_cfg(modversion='glib-2.0')
	
	conf.check(lib='ev', uselib_store='ev', mandatory=True)
	conf.check(header_name='ev.h', uselib='ev', mandatory=True)

	if opts.all:
		opts.lua = True
		opts.openssl = True

	if not opts.debug:
		conf.env['CCFLAGS'] += ['-O2']

	if opts.extra_warnings:
		conf.env['CCFLAGS'] += [
			'-Wmissing-declarations', '-Wdeclaration-after-statement', '-Wno-pointer-sign', '-Wcast-align', '-Winline', '-Wsign-compare',
			'-Wnested-externs', '-Wpointer-arith'#, '-Werror', '-Wbad-function-cast', '-Wmissing-prototypes'
		]
		conf.env['LDFLAGS'] += [
			'-Wmissing-prototypes', '-Wmissing-declarations',
		 	'-Wdeclaration-after-statement', '-Wno-pointer-sign', '-Wcast-align', '-Winline', '-Wsign-compare',
			'-Wnested-externs', '-Wpointer-arith', '-Wl,--as-needed'#, '-Werror', '-Wbad-function-cast'
		]


	if opts.lua:
		# lua has different names in pkg-config on different systems, we do trial and error
		# debian
		if not conf.check_cfg(package='lua5.1', uselib_store='lua', args='--cflags --libs'):
			# freebsd
			if not conf.check_cfg(package='lua-5.1', uselib_store='lua', args='--cflags --libs'):
				# osx
				if not conf.check_cfg(package='lua', uselib_store='lua', args='--cflags --libs'):
					# no pkg-config, try by hand
					conf.env['LIB_lua'] = [ 'm' ]
					conf.check(lib='lua', uselib_store='lua', mandatory=True)
					conf.check(header_name='lua.h', uselib='lua', mandatory=True)
					conf.check(function_name='lua_setfield', header_name='lua.h', uselib='lua', mandatory=True)
		conf.define('HAVE_LUA_H', 1)
		conf.define('HAVE_LIBLUA', 1)
		conf.define('USE_LUA', 1)

	if opts.openssl:
		if not conf.check_cfg(package='openssl', uselib_store='ssl', args='--cflags --libs'):
			conf.check(lib='ssl', uselib_store='ssl', mandatory=True)
			conf.check(header_name='openssl/ssl.h', uselib='ssl', mandatory=True)
			conf.check(lib='crypto', uselib_store='crypto', mandatory=True)
			conf.check(function_name='BIO_f_base64', header_name=['openssl/bio.h','openssl/evp.h'], uselib='crypto', mandatory=True)
			conf.check(function_name='SSL_new', header_name='openssl/ssl.h', uselib='ssl', mandatory=True)
		conf.define('USE_OPENSSL', 1)

	if not opts.static:
		conf.check(lib='dl', uselib_store='dl')
	
	# check for available headers
	conf.check(header_name='sys/socket.h')
	conf.check(header_name='netinet/in.h')
	conf.check(header_name='arpa/inet.h')
	conf.check(header_name='sys/uio.h')
	conf.check(header_name='sys/mman.h')
	conf.check(header_name='sys/sendfile.h')
	conf.check(header_name='sys/un.h')
	
	# check for available functions
	if sys.platform == 'linux2':
		conf.check(function_name='sendfile', header_name='sys/sendfile.h', define_name='HAVE_SENDFILE')
		conf.check(function_name='sendfile64', header_name='sys/sendfile.h', define_name='HAVE_SENDFILE64')
	else:
		conf.check(function_name='sendfile', header_name=['sys/types.h','sys/socket.h','sys/uio.h'], define_name='HAVE_SENDFILE')
	conf.check(function_name='writev', header_name='sys/uio.h', define_name='HAVE_WRITEV')
	conf.check(function_name='inet_aton', header_name='arpa/inet.h', define_name='HAVE_INET_ATON')
	conf.check(function_name='inet_atop', define_name='HAVE_IPV6')
	conf.check(function_name='posix_fadvise', header_name='fcntl.h', define_name='HAVE_POSIX_FADVISE')
	conf.check(function_name='mmap', header_name='sys/mman.h', define_name='HAVE_MMAP')
	conf.check(function_name='fpathconf', header_name='unistd.h', define_name='HAVE_FPATHCONF')
	conf.check(function_name='pathconf', header_name='unistd.h', define_name='HAVE_PATHCONF')
	conf.check(function_name='dirfd', header_name=['sys/types.h', 'dirent.h'], define_name='HAVE_DIRFD')

	conf.sub_config('src/common')
	conf.sub_config('src/angel')
	conf.sub_config('src/main')
	conf.sub_config('src/modules')

	revno = get_revno(conf)
	if revno:
		conf.define('LIGHTTPD_REVISION', revno)

	conf.write_config_header('include/lighttpd/config.h')

	Utils.pprint('WHITE', '----------------------------------------')
	Utils.pprint('BLUE', 'Summary:')
	print_summary(conf, 'Install lighttpd/' + VERSION + ' in', conf.env['PREFIX'])
	if revno:
		print_summary(conf, 'Revision', revno)
	print_summary(conf, 'Using glib version', glib_version)
	print_summary(conf, 'With library directory', opts.libdir)
	print_summary(conf, 'Build static binary', 'yes' if opts.static else 'no', 'YELLOW' if opts.static else 'GREEN')
	print_summary(conf, 'Build debug binary', 'yes' if opts.debug else 'no', 'YELLOW' if opts.debug else 'GREEN')
	print_summary(conf, 'With lua support', 'yes' if opts.lua else 'no', 'GREEN' if opts.lua else 'YELLOW')
	print_summary(conf, 'With ssl support', 'yes' if opts.openssl else 'no', 'GREEN' if opts.openssl else 'YELLOW')
	

def build(bld):
	bld.add_subdirs('src/common')
	bld.add_subdirs('src/angel')
	bld.add_subdirs('src/main')
	bld.add_subdirs('src/modules')
	
def print_summary(conf, msg, result, color = 'GREEN'):
	conf.check_message_1(msg)
	conf.check_message_2(result, color)

def tolist(x):
	if type(x) is types.ListType:
		return x
	return [x]

def get_revno(conf):
	if os.path.exists('.bzr'):
		try:
			revno = Utils.cmd_output('bzr version-info --custom --template="{revno}"')
			if revno:
				return revno.strip()
		except:
			pass
