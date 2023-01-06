# -*- coding: utf-8 -*-

import time
import typing
from hashlib import md5

from pylt.base import ModuleTest
from pylt.requests import CurlRequest, TEST_TXT


def securl(prefix: str, path: str, secret: str, tstamp: typing.Optional[float] = None) -> str:
    if tstamp is None:
        tstamp = time.time()
    hex_tstamp = f'{int(tstamp):x}'
    md5content = secret + path + hex_tstamp
    if prefix[-1] != '/':
        prefix += '/'
    return prefix + md5(md5content.encode()).hexdigest() + '/' + hex_tstamp + path


class SecdownloadFail(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_CODE = 403


class SecdownloadSuccess(CurlRequest):
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200

    def run_test(self) -> bool:
        self.URL = securl('/', '/test.txt', 'abc')
        return super().run_test()


class SecdownloadGone(CurlRequest):
    EXPECT_RESPONSE_CODE = 410

    def run_test(self) -> bool:
        self.URL = securl('/', '/test.txt', 'abc', time.time() - 800)
        return super().run_test()


class Test(ModuleTest):
    config = """
secdownload ( "prefix" => "/", "document-root" => var.default_docroot, "secret" => "abc", "timeout" => 600 );
"""
    no_docroot = True
