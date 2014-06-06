# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestSimple(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 200
	config = """
req_header.overwrite "Host" => "basic-gets";
self_proxy;
"""
	no_docroot = True

class Test(GroupTest):
	group = [
		TestSimple,
	]

	def Prepare(self):
		self.plain_config = """
setup {{ module_load "mod_proxy"; }}

self_proxy = {{
	proxy "127.0.0.2:{self_port}";
}};
""".format(self_port = Env.port)
