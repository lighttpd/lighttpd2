# -*- coding: utf-8 -*-

from base import GroupTest
from requests import CurlRequest
from service import Service
import socket
import os
import base

class SCGI(Service):
	name = "scgi"
	binary = [ None ]

	def __init__(self):
		super(SCGI, self).__init__()
		self.sockfile = os.path.join(base.Env.dir, "tmp", "sockets", self.name + ".sock")
		self.binary = [ os.path.join(base.Env.sourcedir, "run-scgi-envcheck.py") ]

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


class TestPathInfo1(CurlRequest):
	URL = "/scgi/abc/xyz?PATH_INFO"
	EXPECT_RESPONSE_BODY = "/abc/xyz"
	EXPECT_RESPONSE_CODE = 200

class TestRequestUri1(CurlRequest):
	URL = "/scgi/abc/xyz?REQUEST_URI"
	EXPECT_RESPONSE_BODY = "/scgi/abc/xyz?REQUEST_URI"
	EXPECT_RESPONSE_CODE = 200

class Test(GroupTest):
	group = [
		TestPathInfo1,
		TestRequestUri1,
	]

	config = """
run_scgi;
"""

	def FeatureCheck(self):
		scgi = SCGI()
		self.plain_config = """
setup {{ module_load "mod_scgi"; }}

run_scgi = {{
	core.wsgi ( "/scgi", {{ scgi "unix:{socket}"; }} );
}};
""".format(socket = scgi.sockfile)

		self.tests.add_service(scgi)
		return True
