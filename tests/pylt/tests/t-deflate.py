# -*- coding: utf-8 -*-

from pylt.base import GroupTest
from pylt.requests import CurlRequest, TEST_TXT


class DeflateRequest(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200

	EXPECT_RESPONSE_HEADERS = [("Vary", "Accept-Encoding")]

	def Prepare(self):
		self.EXPECT_RESPONSE_HEADERS = self.EXPECT_RESPONSE_HEADERS + [ ("Content-Encoding", self.ACCEPT_ENCODING) ]

class TestGzip(DeflateRequest):
	ACCEPT_ENCODING = 'gzip'


class TestXGzip(DeflateRequest):
	ACCEPT_ENCODING = 'x-gzip'


class TestDeflate(DeflateRequest):
	ACCEPT_ENCODING = 'deflate'


# not supported
#class TestCompress(DeflateRequest):
#	ACCEPT_ENCODING = 'compress'


class TestBzip2(DeflateRequest):
	ACCEPT_ENCODING = 'bzip2'


class TestXBzip2(DeflateRequest):
	ACCEPT_ENCODING = 'x-bzip2'


class TestDisableDeflate(CurlRequest):
	URL = "/test.txt?nodeflate"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200

	EXPECT_RESPONSE_HEADERS = [("Content-Encoding", None)]


class Test(GroupTest):
	group = [TestGzip, TestXGzip, TestDeflate, TestBzip2, TestXBzip2, TestDisableDeflate]

	def Prepare(self):
		# deflate is enabled global too; force it here anyway
		self.config = """
defaultaction;
if req.query == "nodeflate" { req_header.remove "Accept-Encoding"; } static; do_deflate;
"""
