# -*- coding: utf-8 -*-

from pylt.base import ModuleTest
from pylt.requests import CurlRequest


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


class Test(ModuleTest):
    config = """
setup { module_load "mod_dirlist"; }
dirlist;
"""

    def prepare_test(self) -> None:
        self.prepare_dir("www/vhosts/dirlist/foo")
        self.prepare_file("www/vhosts/dirlist/foo/test.txt", "abc")
        self.prepare_dir("www/vhosts/dirlist/f? o")
        self.prepare_file("www/vhosts/dirlist/f? o/test.txt", "abc")
