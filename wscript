#!/usr/bin/env python
# encoding: utf-8

"""
waf build script for Lighttpd 2.x
License and Copyright: see COPYING file
"""

import Options, types, sys, Runner, Utils
from time import gmtime, strftime, timezone

# the following two variables are used by the target "waf dist"
VERSION='2.0-pre'
APPNAME='lighttpd'

# these variables are mandatory ('/' are converted automatically)
srcdir = '.'
blddir = 'build'


def set_options(opt):
	opt.tool_options('compiler_cc')
	opt.tool_options('ragel', tdir = '.')
	
	# ./waf configure options
	opt.add_option('--with-lua', action='store_true', help='with lua 5.1 for mod_magnet [default: off]', dest = 'lua', default = False)
	opt.add_option('--with-all', action='store_true', help='Enable all features', dest = 'all', default = False)
	opt.add_option('--static', action='store_true', help='build a static lighttpd with all modules added', dest = 'static', default = False)
	opt.add_option('--append', action='store', help='Append string to binary names / library dir', dest = 'append', default = '')
	opt.add_option('--lib-dir', action='store', help='Module directory [default: prefix + /lib/lighttpd + append]', dest = 'libdir', default = '')
	opt.add_option('--debug', action='store_true', help='Do not compile with -O2', dest = 'debug', default = False)

def configure(conf):
	opts = Options.options

	conf.define('APPNAME', APPNAME)
	conf.define('VERSION', VERSION)

	conf.check_tool('compiler_cc')
	conf.check_tool('ragel', tooldir = '.')

	conf.env['CCFLAGS'] = tolist(conf.env['CCFLAGS'])
	
	# check for available libraries
	conf.check_cfg(package='glib-2.0', uselib_store='glib', atleast_version='2.16', args='--cflags --libs', mandatory=True)
	conf.check_cfg(package='gmodule-2.0', uselib_store='gmodule', atleast_version='2.16', args='--cflags --libs', mandatory=True)
	conf.check_cfg(package='gthread-2.0', uselib_store='gthread', atleast_version='2.16', args='--cflags --libs', mandatory=True)
	conf.check(header_name="glib.h", uselib='glib', mandatory=True)
	glib_version = conf.check_cfg(modversion='glib-2.0')
	
	conf.check(lib='ev', uselib_store='ev', mandatory=True)
	conf.check(header_name='ev.h', uselib='ev', mandatory=True)
	
	if opts.lua:
		if not conf.check_cfg(package='lua5.1', uselib_store='lua', args='--cflags --libs'):
			conf.env['LIB_lua'] = [ 'm' ]
			conf.check(lib='lua', uselib_store='lua', mandatory=True)
			conf.check(header_name='lua.h', uselib='lua', mandatory=True)
			conf.check(function_name='lua_setfield', header_name='lua.h', uselib='lua', mandatory=True)
		conf.define('HAVE_LUA_H', 1)
		conf.define('HAVE_LIBLUA', 1)
	
	if not opts.static:
		conf.check(lib='dl', uselib_store='dl', mandatory=True)
	
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
	conf.check(function_name='writev', header_name='sys/uio.h', define_name='HAVE_SENDFILE')
	conf.check(function_name='inet_aton', header_name='arpa/inet.h', define_name='HAVE_INET_ATON')
	conf.check(function_name='inet_atop', define_name='HAVE_IPV6')
	conf.check(function_name='posix_fadvise', header_name='fcntl.h', define_name='HAVE_POSIX_FADVISE')
	conf.check(function_name='mmap', header_name='sys/mman.h', define_name='HAVE_MMAP')
	conf.check(function_name='madvise', header_name='sys/mman.h', define_name='HAVE_MADVISE')
	
	conf.env['CPPPATH_lighty'] += [ '../include' ]
	
	conf.env['CPPPATH_lightymod'] += [ '../include' ]
	
	conf.sub_config('src')
	
	conf.define('LIGHTTPD_VERSION_ID', 20000)
	conf.define('PACKAGE_NAME', APPNAME)
	conf.define('PACKAGE_VERSION', VERSION)
	conf.define('PACKAGE_BUILD_DATE', strftime("%b %d %Y %H:%M:%S UTC", gmtime()));
	conf.define('LIBRARY_DIR', opts.libdir)
	conf.define('HAVE_CONFIG_H', 1)
	conf.write_config_header('include/lighttpd/config.h')
	
	Utils.pprint('WHITE', '----------------------------------------')
	Utils.pprint('BLUE', 'Summary:')
	print_summary(conf, 'Install Lighttpd/' + VERSION + ' in', conf.env['PREFIX'])
	print_summary(conf, 'Using glib version', glib_version)
	print_summary(conf, 'With library directory', opts.libdir)
	print_summary(conf, 'Build static binary', 'yes' if opts.static else 'no', 'YELLOW' if opts.static else 'GREEN')
	print_summary(conf, 'Build debug binary', 'yes' if opts.debug else 'no', 'YELLOW' if opts.debug else 'GREEN')
	print_summary(conf, 'With lua support', 'yes' if opts.lua else 'no', 'GREEN' if opts.lua else 'YELLOW')
	

def build(bld):
	bld.add_subdirs('src')
	
	
def print_summary(conf, msg, result, color = 'GREEN'):
	conf.check_message_1(msg)
	conf.check_message_2(result, color)

def tolist(x):
	if type(x) is types.ListType:
		return x
	return [x]

