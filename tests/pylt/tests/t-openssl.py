# -*- coding: utf-8 -*-

from pylt.base import GroupTest
from pylt.requests import CurlRequest, TEST_TXT


class TestSimpleRequest(CurlRequest):
	PORT = 2
	SCHEME = "https"
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]
	vhost = "test1.ssl"


class Test(GroupTest):
	group = [
		TestSimpleRequest
	]
