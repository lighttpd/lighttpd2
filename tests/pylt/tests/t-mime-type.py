# -*- coding: utf-8 -*-

from pylt.base import ModuleTest
from pylt.requests import CurlRequest


class TestMimeType1(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = ""
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain; charset=utf-8")]


class TestMimeType2(CurlRequest):
    URL = "/test.xt"
    EXPECT_RESPONSE_BODY = ""
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/plain")]


class TestMimeType3(CurlRequest):
    URL = "/test.rxt"
    EXPECT_RESPONSE_BODY = ""
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("Content-Type", "text/strange")]


class Test(ModuleTest):
    def prepare_test(self) -> None:
        self.prepare_vhost_file("test.txt", "")
        self.prepare_vhost_file("test.xt", "")
        self.prepare_vhost_file("test.rxt", "")
        self.config = """
mime_types (
    ".txt" => "text/plain; charset=utf-8",
    ".xt" => "text/plain",
    ".rxt" => "text/strange",
    "xt" => "should-not-trigger"
);
"""
