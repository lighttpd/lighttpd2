# -*- coding: utf-8 -*-

import socket
import os

from pylt import base
from pylt.requests import CurlRequest
from pylt.service import Service


class SCGI(Service):
    name = "scgi"
    binary: list[str] = []

    def __init__(self, *, tests: base.Tests) -> None:
        super().__init__(tests=tests)
        self.sockfile = os.path.join(self.tests.env.dir, "tmp", "sockets", self.name + ".sock")
        self.binary = [os.path.join(self.tests.env.sourcedir, "tests", "run-scgi-envcheck.py")]

    def prepare_service(self) -> None:
        assert self.tests
        self.tests.install_dir(os.path.join("tmp", "sockets"))
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.bind(os.path.relpath(self.sockfile))
        sock.listen(8)
        self.fork(*self.binary, inp=sock)

    def cleanup_service(self) -> None:
        assert self.tests
        try:
            os.remove(self.sockfile)
        except Exception as e:
            base.eprint(f"Couldn't delete socket {self.sockfile}: {e}")
        self.tests.remove_dir(os.path.join("tmp", "sockets"))


class TestPathInfo1(CurlRequest):
    URL = "/scgi/abc/xyz?PATH_INFO"
    EXPECT_RESPONSE_BODY = "/abc/xyz"
    EXPECT_RESPONSE_CODE = 200


class TestRequestUri1(CurlRequest):
    URL = "/scgi/abc/xyz?REQUEST_URI"
    EXPECT_RESPONSE_BODY = "/scgi/abc/xyz?REQUEST_URI"
    EXPECT_RESPONSE_CODE = 200


class Test(base.ModuleTest):
    config = """
run_scgi;
"""

    def __init__(self, *, tests: base.Tests) -> None:
        super().__init__(tests=tests)

        scgi = SCGI(tests=self.tests)
        self.plain_config = f"""
setup {{ module_load "mod_scgi"; }}

run_scgi = {{
    core.wsgi ( "/scgi", {{ scgi "unix:{scgi.sockfile}"; }} );
}};
"""
        self.tests.add_service(scgi)
