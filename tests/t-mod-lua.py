# -*- coding: utf-8 -*-

from base import *
from requests import *

LUA_TEST_OPTIONS="""

setup["server.tag"]("lighttpd 2.0 with lua")

function changetag(tag)
	return action["server.tag"](tag)
end

actions = {
	["lua.changetag"] = changetag
}

"""

class TestSetupOption(CurlRequest):
	URL = "/"
	EXPECT_RESPONSE_HEADERS = [("Server", "lighttpd 2.0 with lua")]

class TestChangeOption(CurlRequest):
	URL = "/?change"
	EXPECT_RESPONSE_HEADERS = [("Server", "lighttpd 2.0 with modified lua")]

class Test(GroupTest):
	group = [
		TestSetupOption, TestChangeOption,
	]

	def Prepare(self):
		test_options_lua = self.PrepareFile("lua/test_options.lua", LUA_TEST_OPTIONS)
		self.plain_config = """
				setup {{ lua.plugin "{test_options_lua}"; }}
""".format(test_options_lua = test_options_lua)
		self.config = """
			if req.query == "change" {
				lua.changetag "lighttpd 2.0 with modified lua";
			}
"""
