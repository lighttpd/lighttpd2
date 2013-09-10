# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestAlias1(CurlRequest):
	URL = "/alias1"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
	config = """
alias "/alias1" => var.default_docroot + "/test.txt";
"""

class TestAlias2(CurlRequest):
	URL = "/alias2"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
	config = """
alias "/alias1" => "/nothing", "/alias2" => var.default_docroot + "/test.txt";
"""

class TestAlias3(CurlRequest):
	URL = "/alias3/test.txt"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
	config = """
alias "/alias3" => var.default_docroot + "/";
"""

class Test(GroupTest):
	group = [
		TestAlias1,
		TestAlias2,
		TestAlias3
	]
