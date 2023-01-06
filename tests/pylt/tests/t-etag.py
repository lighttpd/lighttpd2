# -*- coding: utf-8 -*-

import typing
from pylt.requests import CurlRequest, CurlRequestException, TEST_TXT


retrieved_etag1: typing.Optional[str] = None


class TestGetEtag1(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200
    ACCEPT_ENCODING = None

    def CheckResponse(self) -> bool:
        global retrieved_etag1

        retrieved_etag1 = self.response.headers.get('etag')  # lowercase keys!
        if retrieved_etag1 is None:
            raise CurlRequestException("Response missing etag header")
        return super().CheckResponse()


class TestTryEtag1(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = ""
    EXPECT_RESPONSE_CODE = 304
    ACCEPT_ENCODING = None

    def build_headers(self) -> list[str]:
        global retrieved_etag1

        if retrieved_etag1 is None:
            raise CurlRequestException("Don't have a etag value to request")

        headers = super().build_headers()
        headers.append(f"If-None-Match: {retrieved_etag1}")
        return headers

    def CheckResponse(self) -> bool:
        global retrieved_etag1
        etag = self.response.headers.get('etag')  # lowercase keys!
        if etag is None:
            raise CurlRequestException("Response missing etag header")
        if retrieved_etag1 != etag:
            raise CurlRequestException(
                f"Response unexpected etag header response header {etag!r} (wanted {retrieved_etag1!r})"
            )
        return super().CheckResponse()


retrieved_etag2: typing.Optional[str] = None


class TestGetEtag2(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = TEST_TXT
    EXPECT_RESPONSE_CODE = 200

    def CheckResponse(self) -> bool:
        global retrieved_etag2
        retrieved_etag2 = self.response.headers.get('etag', None)  # lowercase keys!
        if retrieved_etag2 is None:
            raise CurlRequestException("Response missing etag header")
        return super().CheckResponse()


class TestTryEtag2(CurlRequest):
    URL = "/test.txt"
    EXPECT_RESPONSE_BODY = ""
    EXPECT_RESPONSE_CODE = 304

    def build_headers(self) -> list[str]:
        global retrieved_etag2

        if retrieved_etag2 is None:
            raise CurlRequestException("Don't have a etag value to request")

        headers = super().build_headers()
        headers.append(f"If-None-Match: {retrieved_etag2}")
        return headers

    def CheckResponse(self) -> bool:
        global retrieved_etag1
        global retrieved_etag2
        etag = self.response.headers.get('etag', None)  # lowercase keys!
        if etag is None:
            raise CurlRequestException("Response missing etag header")
        if retrieved_etag1 == etag:
            raise CurlRequestException(
                f"Response has same etag header as uncompressed response {etag!r} (wanted {retrieved_etag2!r})"
            )
        if retrieved_etag2 != etag:
            raise CurlRequestException(
                f"Response unexpected etag header response header {etag!r} (wanted {retrieved_etag2!r})"
            )
        return super().CheckResponse()
