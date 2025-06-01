# -*- coding: utf-8 -*-

from pylt.requests import CurlRequest


class TestSimpleCidrMap(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = "abc"
    EXPECT_RESPONSE_CODE = 200
    config = """
map_cidr [
    "127.0.0.0/8" => {
    },
    default => {
        respond 402 => "";
    },
];
respond 200 => "abc";
"""
