# -*- coding: utf-8 -*-

from base import *
from requests import *

TEST_TXT="""Hi!
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
"""

class Test(CurlRequest):
	URL = "/test.txt"
	EXPECT_RESPONSE_BODY = TEST_TXT
	EXPECT_RESPONSE_CODE = 200

	def Prepare(self):
		self.PrepareFile("www/default/test.txt", TEST_TXT)
