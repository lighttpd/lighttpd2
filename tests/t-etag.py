# -*- coding: utf-8 -*-

from base import *
from requests import *

TEST_TXT="""Hi!
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"""

retrieved_etag1 = None

class TestGetEtag1(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200
	ACCEPT_ENCODING = None

	def CheckResponse(self):
		global retrieved_etag1
		if not self.resp_headers.has_key('etag'): # lowercase keys!
			raise CurlRequestException("Response missing etag header" % (k, v1, v))
		retrieved_etag1 = self.resp_headers['etag'] # lowercase keys!
		return super(TestGetEtag1, self).CheckResponse()

class TestTryEtag1(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = ""
	EXPECT_RESPONSE_CODE = 304
	ACCEPT_ENCODING = None

	def PrepareRequest(self, reqheaders):
		global retrieved_etag1
		if retrieved_etag1 == None:
			raise CurlRequestException("Don't have a etag value to request")
		c = self.curl
		c.setopt(c.HTTPHEADER, reqheaders + ["If-None-Match: " + retrieved_etag1])

	def CheckResponse(self):
		global retrieved_etag1
		if not self.resp_headers.has_key('etag'): # lowercase keys!
			raise CurlRequestException("Response missing etag header" % (k, v1, v))
		etag = self.resp_headers['etag'] # lowercase keys!
		if retrieved_etag1 != etag:
			raise CurlRequestException("Response unexpected etag header response header '%s' (wanted '%s')" % (etag, retrieved_etag1))
		return super(TestTryEtag1, self).CheckResponse()


retrieved_etag2 = None

class TestGetEtag2(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200

	def CheckResponse(self):
		global retrieved_etag2
		if not self.resp_headers.has_key('etag'): # lowercase keys!
			raise CurlRequestException("Response missing etag header" % (k, v1, v))
		retrieved_etag2 = self.resp_headers['etag'] # lowercase keys!
		return super(TestGetEtag2, self).CheckResponse()

class TestTryEtag2(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = ""
	EXPECT_RESPONSE_CODE = 304

	def PrepareRequest(self, reqheaders):
		global retrieved_etag2
		if retrieved_etag2 == None:
			raise CurlRequestException("Don't have a etag value to request")
		c = self.curl
		c.setopt(c.HTTPHEADER, reqheaders + ["If-None-Match: " + retrieved_etag2])

	def CheckResponse(self):
		global retrieved_etag1
		global retrieved_etag2
		if not self.resp_headers.has_key('etag'): # lowercase keys!
			raise CurlRequestException("Response missing etag header" % (k, v1, v))
		etag = self.resp_headers['etag'] # lowercase keys!
		if retrieved_etag1 == etag:
			raise CurlRequestException("Response has same etag header as uncompressed response '%s' (wanted '%s')" % (etag, retrieved_etag2))
		if retrieved_etag2 != etag:
			raise CurlRequestException("Response unexpected etag header response header '%s' (wanted '%s')" % (etag, retrieved_etag2))
		return super(TestTryEtag2, self).CheckResponse()

class Test(GroupTest):
	group = [TestGetEtag1, TestTryEtag1, TestGetEtag2, TestTryEtag2]
