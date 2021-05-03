# -*- coding: utf-8 -*-

from base import *
from requests import *

class TestDirlist(CurlRequest):
	URL = "/foo/"
	EXPECT_RESPONSE_CODE = 200
	EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/html; charset=utf-8")]

class TestRedirectDir(CurlRequest):
	URL = "/foo"
	EXPECT_RESPONSE_CODE = 301
	EXPECT_RESPONSE_HEADERS = [("Location", "http://dirlist/foo/")]

class TestRedirectDirWithQuery(CurlRequest):
	URL = "/foo?bar=baz"
	EXPECT_RESPONSE_CODE = 301
	EXPECT_RESPONSE_HEADERS = [("Location", "http://dirlist/foo/?bar=baz")]

class TestRedirectDirWithQueryAndSpecialChars(CurlRequest):
	URL = "/f%3f%20o?bar=baz"
	EXPECT_RESPONSE_CODE = 301
	EXPECT_RESPONSE_HEADERS = [("Location", "http://dirlist/f%3f%20o/?bar=baz")]

class Test(GroupTest):
	group = [
		TestDirlist,
		TestRedirectDir,
		TestRedirectDirWithQuery,
		TestRedirectDirWithQueryAndSpecialChars,
	]

	config = """
setup { module_load "mod_dirlist"; }
dirlist;
"""

	def Prepare(self):
		self.PrepareDir("www/vhosts/dirlist/foo")
		self.PrepareFile("www/vhosts/dirlist/foo/test.txt", "abc")
		self.PrepareDir("www/vhosts/dirlist/f? o")
		self.PrepareFile("www/vhosts/dirlist/f? o/test.txt", "abc")
