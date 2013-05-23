# -*- coding: utf-8 -*-

from base import *
from requests import *

LUA_SET_HOST_HEADER="""

function set_host_header(vr)
	vr.req.headers["Host"] = "basic-gets";
end

actions = set_host_header

"""

class TestSimple(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_CODE = 200

	def Prepare(self):
		# we don't want a docroot action this time
		self.vhostdir = None
		self.config = """
set_host_header_basic_gets_lua;
proxy "127.0.0.2:%i";
""" % (Env.port + self.PORT)

class Test(GroupTest):
	group = [
		TestSimple,
	]
	
	def Prepare(self):
		set_host_header_lua = self.PrepareFile("lua/set_host_header.lua", LUA_SET_HOST_HEADER)
		self.plain_config = """
setup {{ module_load "mod_proxy"; }}

set_host_header_basic_gets_lua = {{
	lua.handler "{set_host_header_lua}";
}};
""".format(set_host_header_lua = set_host_header_lua)
