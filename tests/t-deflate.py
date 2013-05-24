# -*- coding: utf-8 -*-

from base import *
from requests import *

TEST_TXT="""Hi!
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"""

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

class Test(GroupTest):
	group = [TestGzip, TestXGzip, TestDeflate, TestBzip2, TestXBzip2]

	def Prepare(self):
		self.PrepareVHostFile("test.txt", TEST_TXT)
		# deflate is enabled global too; force it here anyway
		self.config = """static; do_deflate;"""
