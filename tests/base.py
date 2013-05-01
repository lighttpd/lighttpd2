# -*- coding: utf-8 -*-

"""
Howto write a test case:

Create a file "t-*.py" for your testcase and add a "Test" class to it, like:

from base import *
from requests import *

class Test(CurlRequest):
	...

There are some basic test classes you can derive from:
 * TestBase
 * GroupTest
 * CurlRequest

Each test class can provide Prepare, Run and Cleanup handlers (CurlRequest already provides a Run handler).

A test intance has the following attributes:
 * config: vhost config (error/access log/docroot and vhost handling gets added)
 * plain_config: gets added before all vhost configs (define global actions here)
 * name: unique test name, has a sane default
 * vhost: the vhost name; must be unique if a config is provided;
   GroupTest will set the vhost of subtests to the vhost of the GroupTest
   if the subtest doesn't provide a config
 * runnable: whether to call Run
 * todo: whether the test is expected to fail
 * subdomains: whether a config should be used for (^|\.)vhost (i.e. vhost and all subdomains)

You can create files and directories in Prepare with TestBase.{PrepareVHostFile,PrepareFile,PrepareDir};
they will get removed on cleanup automatically (if the test was successful).

GroupTest:
 * set the group attribute to a list of "subtest" classes (like CurlRequest)

CurlRequest:
set some attributes like:
 * URL = "/test.txt"
 * EXPECT_RESPONSE_CODE = 200
and the class will do everything for you. have a look at the class if you
need more details :)
"""


import os
import imp
import sys
import traceback
import re
import subprocess

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
	return '.'.join(reversed(testname[1:-1].split('/'))).lower()

def regex_subvhosts(vhost):
	return '(^|\\.)' + re.escape(vhost)

# basic interface
class TestBase(object):
	config = None
	plain_config = None
	name = None
	vhost = None
	runnable = True
	todo = False
	subdomains = False # set to true to match all subdomains too

	def __init__(self):
		self._test_cleanup_files = []
		self._test_cleanup_dirs = []
		self._test_failed = False # "not run" is "successful"

	# internal methods, do not override
	def _register(self, tests):
		self.tests = tests
		if not self.vhost: self.vhost = vhostname(self.name)
		self.vhostdir = os.path.join(Env.dir, 'www', 'vhosts', self.vhost)
		if self.FeatureCheck():
			tests.add_test(self)
			return True
		else:
			return False

	def _prepare(self):
		self.Prepare()
		if None != self.plain_config:
			self.tests.append_config(("\n# %s \n" % (self.name)) + self.plain_config)
		if None != self.config:
			errorlog = self.PrepareFile("log/error.log-%s" % self.vhost, "")
			accesslog = self.PrepareFile("log/access.log-%s" % self.vhost, "")
			if None != self.vhostdir:
				docroot = 'docroot "%s";' % self.vhostdir
			else:
				docroot = ''
			if self.subdomains:
				config = """
# %s

var.reg_vhosts = var.reg_vhosts + [ "%s" => {
		log [ "*" => "file:%s" ];
		accesslog "%s";
		%s
%s
	}
];
""" % (self.name, regex_subvhosts(self.vhost), errorlog, accesslog, docroot, self.config)
			else:
				config = """
# %s

var.vhosts = var.vhosts + [ "%s" => {
		log [ "*" => "file:%s" ];
		accesslog "%s";
		%s
%s
	}
];
""" % (self.name, self.vhost, errorlog, accesslog, docroot, self.config)

			self.tests.append_vhosts_config(config)

	def _run(self):
		failed = False
		print >> Env.log, "[Start] Running test %s" % (self.name)
		try:
			if not self.Run():
				failed = True
				if self.todo:
					print >> Env.log, "Test %s failed" % (self.name)
				else:
					print >> sys.stderr, "Test %s failed" % (self.name)
		except BaseException, e:
			failed = True
			if self.todo:
				print >> Env.log, "Test %s failed:" % (self.name)
				print >> Env.log, traceback.format_exc(10)
			else:
				print >> sys.stderr, "Test %s failed: %s" % (self.name, e)
				print >> Env.log, "Test %s failed:" % (self.name)
				print >> Env.log, traceback.format_exc(10)
		print >> Env.log, "[Done] Running test %s [result=%s]" % (self.name, failed and "Failed" or "Succeeded")
		self._test_failed = failed and not self.todo
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
	def PrepareVHostFile(self, fname, content, mode = 0644):
		"""remembers which files have been prepared and while remove them on cleanup; returns absolute pathname"""
		fname = os.path.join('www', 'vhosts', self.vhost, fname)
		return self.PrepareFile(fname, content, mode = mode)

	def PrepareFile(self, fname, content, mode = 0644):
		"""remembers which files have been prepared and while remove them on cleanup; returns absolute pathname"""
		self._test_cleanup_files.append(fname)
		return self.tests.PrepareFile(fname, content, mode = mode)

	def PrepareDir(self, dirname):
		"""remembers which directories have been prepared and while remove them on cleanup; returns absolute pathname"""
		self._test_cleanup_dirs.append(fname)
		return self.tests.PrepareDir(dirname)

	def MissingFeature(self, feature):
		print >> sys.stderr, Env.COLOR_RED + ("Skipping test '%s' due to missing '%s'" % (self.name, feature)) + Env.COLOR_RESET
		return False

	# implement these yourself
	def Prepare(self):
		pass

	def Run(self):
		raise BaseException("Test '%s' not implemented yet" % self.name)

	def Cleanup(self):
		pass

	def FeatureCheck(self):
		return True

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
		if not super(GroupTest, self)._register(tests):
			return False
		for t in self.subtests:
			if None == t.name:
				t.name = self.name + class2testname(t.__class__.__name__) + '/'
			if None == t.vhost and None == t.config:
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
		self.vhosts_config = None
		self.testname_len = 60

		self.prepared_dirs = { }
		self.prepared_files = { }

		self.failed = False

		self.stat_pass = 0
		self.stat_fail = 0
		self.stat_todo = 0
		self.stat_done = 0

		self.add_service(Lighttpd())

	def add_test(self, test):
		name = test.name
		if self.tests_dict.has_key(name):
			raise BaseException("Test '%s' already defined" % name)
		self.tests_dict[name] = test
		if test.runnable:
			for f in self.tests_filter:
				if name.startswith(f):
					self.run.append(test)
					self.testname_len = max(self.testname_len, len(name))
					break
		self.tests.append(test)

	def add_service(self, service):
		service.tests = self
		self.services.append(service)

	def append_config(self, config):
		if None == self.config:
			raise BaseException("Not prepared for adding config")
		self.config += config

	def append_vhosts_config(self, config):
		if None == self.vhosts_config:
			raise BaseException("Not prepared for adding config")
		self.vhosts_config += config

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
	log [ "*" => "stderr" ];

	lua.plugin "{Env.luadir}/core.lua";
	lua.plugin "{Env.luadir}/secdownload.lua";

	accesslog.format "%h %V %u %t \\"%r\\" %>s %b \\"%{{Referer}}i\\" \\"%{{User-Agent}}i\\"";
	accesslog "{accesslog}";
}}

log [ "*" => "file:{errorlog}" ];

defaultaction = {{
	docroot "{Env.defaultwww}";
}};

var.vhosts = [];
var.reg_vhosts = [];
""".format(Env = Env, errorlog = errorlog, accesslog = accesslog)

		self.vhosts_config = ""

		for t in self.tests:
			print >> Env.log, "[Start] Preparing test '%s'" % (t.name)
			t._prepare()

		self.config += self.vhosts_config

		self.config += """

var.reg_vhosts = var.reg_vhosts + [ "default" => {
	defaultaction;
} ];
var.vhosts = var.vhosts + [ "default" => {
	vhost.map_regex var.reg_vhosts;
} ];

vhost.map var.vhosts;
"""
		Env.lighttpdconf = self.PrepareFile("conf/lighttpd.conf", self.config)
		Env.angelconf = self.PrepareFile("conf/angel.conf", """
instance {{
	binary "{Env.worker}";
	config "{Env.lighttpdconf}";
	modules "{Env.plugindir}";

#	env ( "G_SLICE=always-malloc", "G_DEBUG=gc-friendly" );
#	wrapper ("/usr/bin/valgrind", "--leak-check=full", "--show-reachable=yes", "--leak-resolution=high" );
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

	def _progress(self, i, n):
		s = str(n)
		return ("[{0:>%i}" % len(s)).format(i) + "/" + s + "]"

	def Run(self):
		PASS = Env.COLOR_GREEN + "[PASS]" + Env.COLOR_RESET
		FAIL = Env.COLOR_RED + "[FAIL]" + Env.COLOR_RESET
		TODO = Env.COLOR_YELLOW + "[TODO]" + Env.COLOR_RESET
		DONE = Env.COLOR_YELLOW + "[DONE]" + Env.COLOR_RESET

		testcount = len(self.run)
		print >> Env.log, "[Start] Running tests"
		Env.log.flush()
		sys.stdout.flush()
		sys.stderr.flush()

		fmt =  Env.COLOR_BLUE + " {0:<%i}   " % self.testname_len
		failed = False
		i = 1
		for t in self.run:
			result = t._run()
			if t.todo:
				print >> sys.stdout, Env.COLOR_CYAN + self._progress(i, testcount) + fmt.format(t.name) + (result and DONE or TODO)
				if result:
					self.stat_done += 1
				else:
					self.stat_todo += 1
			else:
				print >> sys.stdout, Env.COLOR_CYAN + self._progress(i, testcount) + fmt.format(t.name) + (result and PASS or FAIL)
				if result:
					self.stat_pass += 1
				else:
					self.stat_fail += 1
					failed = True
			i += 1
			Env.log.flush()
			sys.stdout.flush()
			sys.stderr.flush()
		self.failed = failed
		print >> sys.stdout, ("%i out of %i tests passed (%.2f%%), %i tests failed, %i todo items, %i todo items are ready" %
			(self.stat_pass, testcount, (100.0 * self.stat_pass)/testcount, self.stat_fail, self.stat_todo, self.stat_done))
		print >> Env.log, "[Done] Running tests [result=%s]" % (failed and "Failed" or "Succeeded")

		Env.log.flush()
		sys.stdout.flush()
		sys.stderr.flush()

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
	def _preparefile(self, fname, content, mode = 0644):
		if self.prepared_files.has_key(fname):
			raise BaseException("File '%s' already exists!" % fname)
		else:
			path = os.path.join(Env.dir, fname)
			f = open(path, "w")
			f.write(content)
			f.close()
			os.chmod(path, mode)
			self.prepared_files[fname] = 1

	def _cleanupfile(self, fname):
		if self.prepared_files.has_key(fname):
			try:
				os.remove(os.path.join(Env.dir, fname))
			except BaseException, e:
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
			except BaseException, e:
				print >>sys.stderr, "Couldn't delete directory '%s': %s" % (dirname, e)

	def PrepareFile(self, fname, content, mode = 0644):
		path = filter(lambda x: x != '', fname.split('/'))
		for i in range(1, len(path)):
			self._preparedir(os.path.join(*path[0:i]))
		self._preparefile(os.path.join(*path), content, mode = mode)
		return os.path.join(Env.dir, *path)

	def PrepareDir(self, dirname):
		path = filter(lambda x: x != '', dirname.split('/'))
		for i in range(1, len(path)+1):
			self._preparedir(os.path.join(*path[0:i]))
		return os.path.join(Env.dir, *path)

	def CleanupDir(self, dirname):
		path = filter(lambda x: x != '', dirname.split('/'))
		for i in reversed(range(1, len(path)+1)):
			self._cleanupdir(os.path.join(*path[0:i]))

	def CleanupFile(self, fname):
		path = filter(lambda x: x != '', fname.split('/'))
		if not self._cleanupfile(os.path.join(*path)):
			return False
		for i in reversed(range(1, len(path))):
			self._cleanupdir(os.path.join(*path[0:i]))
