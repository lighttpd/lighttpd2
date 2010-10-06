#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os
import sys
from logfile import LogFile, RemoveEscapeSeq

from base import Env, Tests

from optparse import OptionParser

from tempfile import mkdtemp

def find_port(port):
	if port >= 1024 and port < (65536-8):
		return port

	from random import Random
	r = Random(os.getpid())
	return r.randint(1024, 65536-8)

class ArgumentError(Exception):
	def __init__(self, value): self.value = value
	def __str__(self): return repr(self.value)

parser = OptionParser()
parser.add_option("--angel", help = "Path to angel binary (required)")
parser.add_option("--worker", help = "Path to worker binary (required)")
parser.add_option("--plugindir", help = "Path to plugin directory (required)")
parser.add_option("-k", "--no-cleanup", help = "Keep temporary files, no cleanup", action = "store_true", default = False)
parser.add_option("-p", "--port", help = "Use [port,port+7] as tcp ports on 127.0.0.1 (default: 8088; use 0 for random port)", default = 8088, type = "int")
parser.add_option("-t", "--test", help = "Run specific test", action = "append", dest = "tests", default = [])
parser.add_option("-c", "--force-cleanup", help = "Keep no temporary files (overwrites -k)", action = "store_true", default = False)
parser.add_option("--strace", help = "Strace services", action = "store_true", default = False)
parser.add_option("--truss", help = "Truss services", action = "store_true", default = False)
parser.add_option("--debug-requests", help = "Dump requests", action = "store_true", default = False)
parser.add_option("--no-angel", help = "Spawn lighttpd worker directly", action = "store_true", default = False)

(options, args) = parser.parse_args()

if not options.angel or not options.worker or not options.plugindir:
	raise ArgumentError("Missing required arguments")

if options.force_cleanup: options.no_cleanup = False

Env.angel = os.path.abspath(options.angel)
Env.worker = os.path.abspath(options.worker)
Env.plugindir = os.path.abspath(options.plugindir)
Env.no_cleanup = options.no_cleanup
Env.force_cleanup = options.force_cleanup
Env.port = find_port(options.port)
Env.tests = options.tests
Env.sourcedir = os.path.abspath(os.path.dirname(__file__))
Env.luadir = os.path.join(os.path.dirname(Env.sourcedir), "doc")
Env.debugRequests = options.debug_requests
Env.strace = options.strace
Env.truss = options.truss
Env.color = sys.stdin.isatty()
Env.no_angel = options.no_angel

Env.dir = mkdtemp(dir = os.getcwd())
Env.defaultwww = os.path.join(Env.dir, "www", "default")

Env.log = open(os.path.join(Env.dir, "tests.log"), "w")
if Env.color: Env.log = RemoveEscapeSeq(Env.log)
sys.stderr = LogFile(sys.stderr, **{ "[stderr]": Env.log })
sys.stdout = LogFile(sys.stdout, **{ "[stdout]": Env.log })
Env.log = LogFile(Env.log)

failed = False

try:
	# run tests
	tests = Tests()
	tests.LoadTests()
	failed = True
	try:
		tests.Prepare()
	except:
		raise
	else:
		if tests.Run():
			failed = False
	finally:
		tests.Cleanup()
		if not Env.no_cleanup and not failed:
			os.remove(os.path.join(Env.dir, "tests.log"))

finally:
	try:
		if Env.force_cleanup:
			import shutil
			shutil.rmtree(Env.dir)
		elif not Env.no_cleanup and not failed:
			os.rmdir(Env.dir)
	except OSError:
		print >> sys.stderr, "Couldn't delete temporary directory '%s', probably not empty (perhaps due to some errors)" % Env.dir

Env.log.close()

if failed:
	sys.exit(1)
else:
	sys.exit(0)
