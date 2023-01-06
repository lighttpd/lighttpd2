# -*- coding: utf-8 -*-

from pylt.base import eprint, log
from pylt.requests import CurlRequest


class TestHeaderAdd(CurlRequest):
    # use capture from previous regex conditional
    config = """
header.add "Test-Header" => "%{req.query}";
header.add "Test-Header" => "%{req.path}";
respond;
"""
    URL = "/path?simple_query"

    def CheckResponse(self) -> bool:
        h = self.response.get_all_headers("Test-Header")
        if len(h) != 2 or h[0] != "simple_query" or h[1] != "/path":
            eprint(repr(h))
            raise Exception("Unexpected headers 'Test-Header'")
        return True


class TestHeaderAppend(CurlRequest):
    # use capture from previous regex conditional
    config = """
header.append "Test-Header" => "%{req.query}";
header.append "Test-Header" => "%{req.path}";
respond;
"""
    URL = "/path?simple_query"

    def CheckResponse(self) -> bool:
        h = self.response.get_all_headers("Test-Header")
        if len(h) != 1 or h[0] != "simple_query, /path":
            log(repr(h))
            raise Exception("Unexpected headers 'Test-Header'")
        return True


class TestHeaderOverwrite(CurlRequest):
    # use capture from previous regex conditional
    config = """
header.overwrite "Test-Header" => "%{req.query}";
header.overwrite "Test-Header" => "%{req.path}";
respond;
"""
    URL = "/path?simple_query"

    def CheckResponse(self) -> bool:
        h = self.response.get_all_headers("Test-Header")
        if len(h) != 1 or h[0] != "/path":
            log(repr(h))
            raise Exception("Unexpected headers 'Test-Header'")
        return True


class TestHeaderRemove(CurlRequest):
    # use capture from previous regex conditional
    config = """
header.add "Test-Header" => "%{req.query}";
header.remove "Test-Header";
respond;
"""
    URL = "/path?simple_query"

    def CheckResponse(self) -> bool:
        h = self.response.get_all_headers("Test-Header")
        if len(h) != 0:
            log(repr(h))
            raise Exception("Unexpected headers 'Test-Header'")
        return True
