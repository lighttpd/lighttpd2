# -*- coding: utf-8 -*-

import os
import imp
import sys
import traceback

from service import *

__all__ = [ "Env", "Tests", "TestBase", "GroupTest" ]

class Dict(object):
	pass

Env = Dict()


def fix_test_name(name):
	if None == name: return '/'
	if (name[:1] != '/'): name = '/' + name
	if (name[-1:] != '/'): name = name + '/'
	return name

def load_test_file(name):
	path = os.path.join(Env.sourcedir, name)
	file = open(path)
	(modname, ext) = os.path.splitext(name)
	module = imp.load_module(modname, file, path, (ext, 'r', imp.PY_SOURCE))
	file.close()
	return module

def vhostname(testname):
	return testname[1:-1].replace('/', '.')

# basic interface
class TestBase(object):
	config = None
	name = None
	vhost = None
	runnable = True

	def __init__(self):
		self._test_cleanup_files = []
		self._test_cleanup_dirs = []
		self._test_failed = False # "not run" is "successful"

	# internal methods, do not override
	def _register(self, tests):
		self.tests = tests
		if not self.vhost: self.vhost = vhostname(self.name)
		self.vhostdir = os.path.join(Env.dir, 'www', 'vhosts', self.vhost)
		tests.add_test(self)

	def _prepare(self):
		self.Prepare()
		if None != self.config:
			errorlog = self.PrepareFile("log/error.log-%s" % self.vhost, "")
			accesslog = self.PrepareFile("log/access.log-%s" % self.vhost, "")
			config = """
var.vhosts = var.vhosts + [ "%s" : ${
		log = [ "*": "file:%s" ];
		accesslog = "%s";
%s
	}
];
""" % (self.vhost, errorlog, accesslog, self.config)
			self.tests.append_config(config)

	def _run(self):
		failed = False
		print >> Env.log, "[Start] Running test %s" % (self.name)
		try:
			if not self.Run():
				failed = True
				print >> sys.stderr, "Test %s failed" % (self.name)
		except Exception as e:
			failed = True
			print >> sys.stderr, "Test %s failed:" % (self.name)
			print >> sys.stderr, traceback.format_exc(10)
		print >> Env.log, "[Done] Running test %s [result=%s]" % (self.name, failed and "Failed" or "Succeeded")
		self._test_failed = failed
		return not failed

	def _cleanup(self):
		if Env.no_cleanup or (not Env.force_cleanup and self._test_failed):
			return
		self.Cleanup()
		for f in self._test_cleanup_files:
			self._cleanupFile(f)
		for d in self._test_cleanup_dirs:
			self._cleanupDir(d)
		self._test_cleanup_files = []
		self._test_cleanup_dirs = []

	def _cleanupFile(self, fname):
		self.tests.CleanupFile(fname)

	def _cleanupDir(self, dirname):
		self.tests.CleanupDir(dirname)

	# public
	def PrepareVHostFile(self, fname, content):
		"""remembers which files have been prepared and while remove them on cleanup; returns absolute pathname"""
		fname = 'www/vhosts/' + self.vhost + '/' + fname
		return self.tests.PrepareFile(fname, content)

	def PrepareFile(self, fname, content):
		"""remembers which files have been prepared and while remove them on cleanup; returns absolute pathname"""
		self._test_cleanup_files.append(fname)
		return self.tests.PrepareFile(fname, content)

	def PrepareDir(self, dirname):
		"""remembers which directories have been prepared and while remove them on cleanup; returns absolute pathname"""
		self._test_cleanup_dirs.append(fname)
		return self.tests.PrepareDir(dirname)


	# implement these yourself
	def Prepare(self):
		pass

	def Run(self):
		raise BaseException("Test '%s' not implemented yet" % self.name)

	def Cleanup(self):
		pass

def class2testname(name):
	if name.startswith("Test"): name = name[4:]
	return name

class GroupTest(TestBase):
	runnable = False

	def __init__(self):
		super(GroupTest, self).__init__()
		self.subtests = []
		for c in self.group:
			t = c()
			self.subtests.append(t)

	def _register(self, tests):
		super(GroupTest, self)._register(tests)
		for t in self.subtests:
			if None == t.name:
				t.name = self.name + class2testname(t.__class__.__name__) + '/'
			if None == t.vhost:
				t.vhost = self.vhost
			t._register(tests)

	def _cleanup(self):
		for t in self.subtests:
			if t._test_failed:
				self._test_failed = True
		super(GroupTest, self)._cleanup()


class Tests(object):
	def __init__(self):
		self.tests_filter = []
		if 0 == len(Env.tests):
			self.tests_filter.append("")
		else:
			for t in Env.tests:
				self.tests_filter.append(fix_test_name(t))

		self.services = []
		self.run = [] # tests we want to run
		self.tests = [] # all tests (we always prepare/cleanup all tests)
		self.tests_dict = { }
		self.config = None

		self.prepared_dirs = { }
		self.prepared_files = { }

		self.failed = False

		self.add_service(Lighttpd())

	def add_test(self, test):
		name = test.name
		if self.tests_dict.has_key(name):
			raise BaseException("Test '%s' already defined" % name)
		self.tests_dict[name] = test
		for f in self.tests_filter:
			if name.startswith(f):
				if test.runnable:
					self.run.append(test)
				break
		self.tests.append(test)

	def add_service(self, service):
		service.tests = self
		self.services.append(service)

	def append_config(self, config):
		if None == self.config:
			raise BaseException("Not prepared for adding config")
		self.config += config

	def LoadTests(self):
		files = os.listdir(Env.sourcedir)
		files = filter(lambda x: x[-3:] == '.py', files)
		files = filter(lambda x: x[:2] == 't-', files)
		files.sort()

		mods = []
		for f in files:
			mods.append(load_test_file(f))

		for m in mods:
			t = m.Test()
			t.name = fix_test_name(t.name)
			if '/' == t.name:
				(n, _) = os.path.splitext(os.path.basename(m.__file__))
				t.name = fix_test_name(n[2:])
			t._register(self)

	def Prepare(self):
		print >> Env.log, "[Start] Preparing tests"
		errorlog = self.PrepareFile("log/error.log", "")
		accesslog = self.PrepareFile("log/access.log", "")
		self.config = """
setup {{
	workers 2;

	module_load (
		"mod_accesslog",
		"mod_dirlist",
		"mod_lua",
		"mod_vhost"
	);

	listen "127.0.0.1:{Env.port}";
	log = [ "*": "stderr" ];

	lua.plugin "{Env.luadir}/core.lua";
	lua.plugin "{Env.luadir}/secdownload.lua";

	accesslog.format = "%h %V %u %t \\"%r\\" %>s %b \\"%{{Referer}}i\\" \\"%{{User-Agent}}i\\"";
	accesslog = "{accesslog}";
}}

log = [ "*": "file:{errorlog}" ];

defaultaction {{
	docroot "{Env.defaultwww}";
}}

var.vhosts = [ "default": ${{
	defaultaction;
}} ];
""".format(Env = Env, errorlog = errorlog, accesslog = accesslog)

		for t in self.tests:
			print >> Env.log, "[Start] Preparing test '%s'" % (t.name)
			t._prepare()

		self.config += """
vhost.map var.vhosts;
"""
		Env.lighttpdconf = self.PrepareFile("conf/lighttpd.conf", self.config)
		Env.angelconf = self.PrepareFile("conf/angel.conf", """
instance {{
	binary "{Env.worker}";
	config "{Env.lighttpdconf}";
	modules "{Env.plugindir}";
}}

allow-listen {{ ip "127.0.0.1:{Env.port}"; }}
""".format(Env = Env))

		print >> Env.log, "[Done] Preparing tests"

		print >> Env.log, "[Start] Preparing services"
		for s in self.services:
			try:
				s._prepare()
			except:
				self.failed = True
				raise
		print >> Env.log, "[Done] Preparing services"


	def Run(self):
		print >> Env.log, "[Start] Running tests"
		failed = False
		for t in self.run:
			if not t._run(): failed = True
		self.failed = failed
		print >> Env.log, "[Done] Running tests [result=%s]" % (failed and "Failed" or "Succeeded")
		return not failed

	def Cleanup(self):
#		print >> sys.stderr, "cleanup_files: %s, cleanup_dirs: %s" % (self.prepared_files, self.prepared_dirs)

		if not Env.no_cleanup and not self.failed:
			print >> Env.log, "[Start] Cleanup services"
			for s in self.services:
				s._cleanup()
			print >> Env.log, "[Done] Cleanup services"
		else:
			print >> Env.log, "[Start] Stopping services"
			for s in self.services:
				s._stop()
			print >> Env.log, "[Done] Stopping services"

		print >> Env.log, "[Start] Cleanup tests"
		for t in self.tests:
			t._cleanup()
		if not Env.no_cleanup and not self.failed:
			self.CleanupFile("log/access.log")
			self.CleanupFile("log/error.log")
			self.CleanupFile("conf/lighttpd.conf")
			self.CleanupFile("conf/angel.conf")
		print >> Env.log, "[Done] Cleanup tests"

## helpers for prepare/cleanup
	def _preparefile(self, fname, content):
		if self.prepared_files.has_key(fname):
			raise BaseException("File '%s' already exists!" % fname)
		else:
			f = open(os.path.join(Env.dir, fname), "w")
			f.write(content)
			f.close()
			self.prepared_files[fname] = 1

	def _cleanupfile(self, fname):
		if self.prepared_files.has_key(fname):
			try:
				os.remove(os.path.join(Env.dir, fname))
			except Exception as e:
				print >>sys.stderr, "Couldn't delete file '%s': %s" % (fname, e)
				return False
			return True
		else:
			return False

	def _preparedir(self, dirname):
		if self.prepared_dirs.has_key(dirname):
			self.prepared_dirs[dirname] += 1
		else:
			os.mkdir(os.path.join(Env.dir, dirname))
			self.prepared_dirs[dirname] = 1

	def _cleanupdir(self, dirname):
		self.prepared_dirs[dirname] -= 1
		if 0 == self.prepared_dirs[dirname]:
			try:
				os.rmdir(os.path.join(Env.dir, dirname))
			except Exception as e:
				print >>sys.stderr, "Couldn't delete directory '%s': %s" % (dirname, e)

	def PrepareFile(self, fname, content):
		path = filter(lambda x: x != '', fname.split('/'))
		for i in range(1, len(path)):
			self._preparedir(os.path.join(*path[0:i]))
		self._preparefile(os.path.join(*path), content)
		return os.path.join(Env.dir, *path)

	def PrepareDir(self, dirname):
		path = filter(lambda x: x != '', fname.split('/'))
		for i in range(1, len(path)+1):
			self._preparedir(os.path.join(*path[0:i]))
		return os.path.join(Env.dir, *path)

	def CleanupDir(self, dirname):
		path = filter(lambda x: x != '', fname.split('/'))
		for i in reversed(range(1, len(path)+1)):
			self._cleanupdir(os.path.join(*path[0:i]))

	def CleanupFile(self, fname):
		path = filter(lambda x: x != '', fname.split('/'))
		if not self._cleanupfile(os.path.join(*path)):
			return False
		for i in reversed(range(1, len(path))):
			self._cleanupdir(os.path.join(*path[0:i]))


class Lighttpd(Service):
	name = "lighttpd"

	def Prepare(self):
		self.portfree(Env.port)
		self.fork(Env.angel, '-m', Env.plugindir, '-c', Env.angelconf)
		self.waitconnect(Env.port)
