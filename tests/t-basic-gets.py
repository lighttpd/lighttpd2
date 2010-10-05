# -*- coding: utf-8 -*-

from base import *
from requests import *

TEST_TXT="""Hi!
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"""

LUA_SHOW_ENV_INFO="""

function show_env_info(vr)
	if vr:handle_direct() then
		vr.resp.status = 200
		vr.resp.headers["Content-Type"] = "text/plain"
		vr.out:add(vr.env["INFO"])
	end
end

actions = show_env_info

"""

class TestSimpleRequest(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200

class TestSimpleInfo(CurlRequest):
	URL = "/?a_simple_query"
	EXPECT_RESPONSE_BODY = "a_simple_query"
	EXPECT_RESPONSE_CODE = 200

	config = """
env.set "INFO" => "%{req.query}";
show_env_info;
"""

class Test(GroupTest):
	group = [TestSimpleRequest,TestSimpleInfo]

	def Prepare(self):
		self.PrepareFile("www/default/test.txt", TEST_TXT)
		show_env_info_lua = self.PrepareFile("lua/show_env_info.lua", LUA_SHOW_ENV_INFO)
		self.plain_config = """
show_env_info {{
	lua.handler "{show_env_info_lua}";
}}
""".format(show_env_info_lua = show_env_info_lua)
