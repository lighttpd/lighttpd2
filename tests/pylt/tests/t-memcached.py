# -*- coding: utf-8 -*-

import socket
import os
import time

from pylt import base
from pylt.requests import CurlRequest
from pylt.service import Service


class Memcached(Service):
    name = "memcached"
    binary: list[str] = []

    def __init__(self, *, tests: base.Tests) -> None:
        super().__init__(tests=tests)
        self.sockfile = os.path.join(self.tests.env.dir, "tmp", "sockets", self.name + ".sock")
        self.binary = [os.path.join(self.tests.env.sourcedir, "tests", "run-memcached.py")]

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
            base.eprint(f"Couldn't delete socket {self.sockfile!r}: {e}")
        self.tests.remove_dir(os.path.join("tmp", "sockets"))


class TestStore1(CurlRequest):
    URL = "/"
    EXPECT_RESPONSE_BODY = "Hello World!"
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("X-Memcached-Hit", "false")]


class TestLookup1(CurlRequest):
    URL = "/"
    EXPECT_RESPONSE_BODY = "Hello World!"
    EXPECT_RESPONSE_CODE = 200
    EXPECT_RESPONSE_HEADERS = [("X-Memcached-Hit", "true")]

    def run_test(self) -> bool:
        # storing might take some time: only after the request is actually
        # finished does lighttpd start the memcache connection to store it
        time.sleep(0.2)
        return super().run_test()


class Test(base.ModuleTest):
    config = """
memcache;
"""

    def __init__(self, *, tests: base.Tests) -> None:
        super().__init__(tests=tests)

        memcached = Memcached(tests=self.tests)
        self.plain_config = f"""
setup {{ module_load "mod_memcached"; }}

memcache = {{
    memcached.lookup (( "server" => "unix:{memcached.sockfile}" ), {{
            header.add "X-Memcached-Hit" => "true";
        }}, {{
            header.add "X-Memcached-Hit" => "false";
            respond 200 => "Hello World!";
            memcached.store ( "server" => "unix:{memcached.sockfile}" );
        }});
}};
"""
        self.tests.add_service(memcached)
