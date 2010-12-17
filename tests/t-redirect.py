# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestForceHttps(CurlRequest):
	config = """
redirect "https://%{request.host}%{enc:request.path}?%{request.query}";
"""
	URL = "/?a_simple_query"
	EXPECT_RESPONSE_CODE = 301
	EXPECT_RESPONSE_HEADERS = [("Location", "https://forcehttps.redirect/?a_simple_query")]

class TestRemoveWWW(CurlRequest):
	config = """
if req.host =~ "^www\.(.+)$" {
	redirect "http://%1%{enc:request.path}?%{request.query}";
}
"""

	vhost = "www.test"
	URL = "/foo?bar"
	EXPECT_RESPONSE_CODE = 301
	EXPECT_RESPONSE_HEADERS = [("Location", "http://test/foo?bar")]

class Test(GroupTest):
	plain_config = """
setup { module_load "mod_redirect"; }
"""

	group = [
		TestForceHttps,
		TestRemoveWWW
	]
