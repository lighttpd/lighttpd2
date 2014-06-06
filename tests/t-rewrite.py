# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestRewrite1(CurlRequest):
	URL = "/somefile"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
	config = """
rewrite "^/somefile$" => "/test.txt";
defaultaction;
"""

class TestRewrite2(CurlRequest):
	URL = "/somefile"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
	config = """
rewrite "/somethingelse" => "/nothing", "^/somefile$" => "/test.txt";
defaultaction;
"""

# match decoded and simplified paths by default
class TestRewrite3(CurlRequest):
	URL = "/http://some%2Ffile"
	EXPECT_RESPONSE_BODY = "/dest/file"
	EXPECT_RESPONSE_CODE = 200
	config = """
rewrite "/http:/some(/.*)" => "/dest$1";
respond 200 => "%{req.path}";
"""

# match raw paths and simplify path
class TestRewrite4(CurlRequest):
	URL = "/http://some%2Ffile"
	EXPECT_RESPONSE_BODY = "/dest/http:/some/file"
	EXPECT_RESPONSE_CODE = 200
	config = """
rewrite_raw "(/http://some%2F.*)" => "/dest$1";
respond 200 => "%{req.path}";
"""

# match and write raw paths
class TestRewrite5(CurlRequest):
	URL = "/http://some%2Ffile"
	EXPECT_RESPONSE_BODY = "/dest/http://some%2Ffile"
	EXPECT_RESPONSE_CODE = 200
	config = """
rewrite_raw "(/http://some%2F.*)" => "/dest$1";
respond 200 => "%{req.raw_path}";
"""

# raw match and write query string
class TestRewrite6(CurlRequest):
	URL = "/http://some%2Ffile"
	EXPECT_RESPONSE_BODY = "/http://some%2Ffile"
	EXPECT_RESPONSE_CODE = 200
	config = """
rewrite_raw "(/http://some%2F.*)" => "/dest?$1";
respond 200 => "%{req.query}";
"""

class Test(GroupTest):
	plain_config = """
setup { module_load "mod_rewrite"; }
"""

	group = [
		TestRewrite1,
		TestRewrite2,
		TestRewrite3,
		TestRewrite4,
		TestRewrite5,
		TestRewrite6,
	]
