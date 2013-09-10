# -*- coding: utf-8 -*-

from base import GroupTest
from requests import CurlRequest
from service import Service
import socket
import os
import base


class Memcached(Service):
	name = "memcached"
	binary = [ None ]

	def __init__(self):
		super(Memcached, self).__init__()
		self.sockfile = os.path.join(base.Env.dir, "tmp", "sockets", self.name + ".sock")
		self.binary = [ os.path.join(base.Env.sourcedir, "tests", "run-memcached.py") ]


	def Prepare(self):
		sockdir = self.tests.PrepareDir(os.path.join("tmp", "sockets"))
		sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		sock.bind(os.path.relpath(self.sockfile))
		sock.listen(8)
		self.fork(*self.binary, inp = sock)

	def Cleanup(self):
		if None != self.sockfile:
			try:
				os.remove(self.sockfile)
			except BaseException, e:
				print >>sys.stderr, "Couldn't delete socket '%s': %s" % (self.sockfile, e)
		self.tests.CleanupDir(os.path.join("tmp", "sockets"))

class TestStore1(CurlRequest):
	URL = "/"
	EXPECT_RESPONSE_BODY = "Hello World!"
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("X-Memcached-Hit", "false")]

class TestLookup1(CurlRequest):
	URL = "/"
	EXPECT_RESPONSE_BODY = "Hello World!"
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("X-Memcached-Hit", "true")]

class Test(GroupTest):
	group = [
		TestStore1,
		TestLookup1,
	]

	config = """
memcache;
"""

	def FeatureCheck(self):
		memcached = Memcached()
		self.plain_config = """
setup {{ module_load "mod_memcached"; }}

memcache = {{
	memcached.lookup (( "server" => "unix:{socket}" ), {{
			header.add "X-Memcached-Hit" => "true";
		}}, {{
			header.add "X-Memcached-Hit" => "false";
			respond 200 => "Hello World!";
			memcached.store ( "server" => "unix:{socket}" );
		}});
}};
""".format(socket = memcached.sockfile)

		self.tests.add_service(memcached)
		return True
