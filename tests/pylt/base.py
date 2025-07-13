# -*- coding: utf-8 -*-

from __future__ import annotations
import contextlib
import contextvars
import dataclasses
import textwrap
import types
import io

"""
Howto write a test case:

Create a file "t-*.py" for your testcase and add a "Test" class to it, like:

from base import ModuleTest
from requests import CurlRequest

class Test(CurlRequest):
    ...

There are some basic test classes you can derive from:
 * TestBase
 * ModuleTest
 * CurlRequest

Each test class can provide Prepare, Run and Cleanup handlers (CurlRequest already provides a Run handler).

A test instance has the following attributes:
 * config: vhost config (error/access log/docroot and vhost handling gets added)
 * plain_config: gets added before all vhost configs (define global actions here)
 * name: unique test name, has a sane default
 * vhost: the vhost name; must be unique if a config is provided;
   ModuleTest will set the vhost of subtests to the vhost of the ModuleTest
   if the subtest doesn't provide a config
 * runnable: whether to call Run
 * todo: whether the test is expected to fail
 * subdomains: whether a config should be used for (^|\\.)vhost (i.e. vhost and all subdomains)

You can create files and directories in Prepare with TestBase.{prepare_vhost_file,prepare_file,prepare_dir};
they will get removed on cleanup automatically (if the test was successful).

class Test(ModuleTest):
 * basic config for all tests in a module
 * optional

CurlRequest:
set some attributes like:
 * URL = "/test.txt"
 * EXPECT_RESPONSE_CODE = 200
and the class will do everything for you. have a look at the class if you
need more details :)
"""

import importlib
import os
import re
import sys
import traceback
import typing


if typing.TYPE_CHECKING:
    from pylt.service import Service


__all__ = ["Tests", "TestBase", "ModuleTest", "eprint", "log"]


@dataclasses.dataclass
class Env:
    angel: str = ''
    worker: str = ''
    plugindir: str = ''
    no_cleanup: bool = False
    force_cleanup: bool = False
    port: int = 0
    tests: list[str] = dataclasses.field(default_factory=list)
    sourcedir: str = ''
    contribdir: str = ''
    debugRequests: bool = False
    strace: bool = False
    truss: bool = False
    no_angel: bool = False
    debug: bool = False
    wait: bool = False
    valgrind: str = ''
    valgrind_leak: str = ''
    color: bool = False
    COLOR_RESET: str = ''
    COLOR_BLUE: str = ''
    COLOR_GREEN: str = ''
    COLOR_YELLOW: str = ''
    COLOR_RED: str = ''
    COLOR_CYAN: str = ''
    dir: str = ''
    defaultwww: str = ''
    log: io.TextIOBase = dataclasses.field(default_factory=io.StringIO)
    stdout: typing.Union[io.TextIOBase, typing.IO[str]] = sys.stdout
    stderr: typing.Union[io.TextIOBase, typing.IO[str]] = sys.stderr
    #
    lighttpdconf: str = ''
    angelconf: str = ''

    @contextlib.contextmanager
    def use(self) -> typing.Generator[None, None, None]:
        token = EnvContext.set(self)
        try:
            yield
        finally:
            EnvContext.reset(token)


# the log/print methods below are just way easier to use without
# explicit env context; with ContextVar those still are
# only semi global.
EnvContext: contextvars.ContextVar[typing.Optional[Env]] = contextvars.ContextVar('env', default=None)


def log(*args, **kwargs) -> None:
    env = EnvContext.get()
    file = env and env.log or sys.stderr
    print(*args, file=file, **kwargs)


def print_stdout(*args, **kwargs) -> None:
    env = EnvContext.get()
    file = env and env.stdout or sys.stdout
    print(*args, file=file, **kwargs)


def eprint(*args, **kwargs) -> None:
    env = EnvContext.get()
    file = env and env.stderr or sys.stderr
    print(*args, file=file, **kwargs)


def _get_mod_test_list(mod: types.ModuleType) -> list[type[TestBase]]:
    ex_l = getattr(mod, '_pylt_test_list', None)
    if not ex_l is None:
        return typing.cast(list[type[TestBase]], ex_l)
    newl: list[type[TestBase]] = []
    setattr(mod, '_pylt_test_list', newl)
    return newl


def register_test_in_module(test: type[TestBase]):
    """This is usually called automatically for classes inheriting from TestBase"""
    test_list = _get_mod_test_list(sys.modules[test.__module__])
    test_list.append(test)


class _TestBaseMeta(type):
    NEVER_REGISTER = False  # inherited for metaclass

    def __new__(
        cls: type[_TestBaseMeta],
        clsname: str,
        bases: tuple[type, ...],
        attrs: dict, **kwargs,
    ) -> _TestBaseMeta:
        no_register = attrs.pop('_NO_REGISTER', False)
        result = type.__new__(cls, clsname, bases, attrs, **kwargs)
        if not no_register and not result.NEVER_REGISTER:
            assert issubclass(result, TestBase)
            register_test_in_module(result)
        return result


# basic interface
class TestBase(metaclass=_TestBaseMeta):
    _NO_REGISTER = True  # only for metaclass

    config: str = ''
    plain_config: str = ''
    name: str = ''
    vhost: str = ''
    runnable = True
    todo = False
    subdomains = False  # set to true to match all subdomains too
    inherit_docroot = False
    no_docroot = False
    vhostdir: str = ''

    @staticmethod
    def _vhostname(testname: str) -> str:
        return '.'.join(reversed(testname[1:-1].split('/'))).lower()

    def __init__(self, *, tests: Tests, parent: typing.Optional[TestBase] = None) -> None:
        self.tests = tests
        self._test_cleanup_files: list[str] = []
        self._test_cleanup_dirs: list[str] = []
        self._test_failed = False  # "not run" is "successful"
        self._parent: typing.Optional[TestBase] = parent
        self._errorlog: typing.Optional[str] = None  # file to print to log when test fails

        if not self.name:
            if parent:
                parent_name = parent.name
            else:
                parent_name = '/'
            self.name = parent_name + class2testname(self.__class__.__name__) + '/'
        if parent and not self.vhost and not self.config:
            # if we have a config we need a dedicated vhost name to run it with.
            # but if there is no custom config we should use the parent vhost.
            self.vhost = parent.vhost
        if not self.vhost:
            self.vhost = TestBase._vhostname(self.name) + '.test'
        if not self.no_docroot:
            if self.inherit_docroot:
                assert self._parent, "can't inherit vhostdir without parent"
                assert self._parent.vhostdir, "can't inherit unset vhostdir"
                self.vhostdir = self._parent.vhostdir
            else:
                self.vhostdir = os.path.join(self.tests.env.dir, 'www', 'vhosts', self.vhost)
        tests._add_test(self)

    # internal methods, do not override
    def _prepare(self) -> None:
        self.prepare_test()
        if self.plain_config:
            self.tests.append_config(f"\n# {self.name} \n{self.plain_config}")
        if self.config:
            errorlog = self.prepare_file(f"log/error.log-{self.vhost}", "")
            self._errorlog = errorlog
            errorconfig = self.tests.env.debug and " " or f"""log [ default => "file:{errorlog}" ];"""
            accesslog = self.prepare_file(f"log/access.log-{self.vhost}", "")
            if self.vhostdir:
                docroot = f'local var.docroot = "{self.vhostdir}"; docroot var.docroot;'
            else:
                docroot = ''
            if self.subdomains:
                vhost_regex = r'(^|\.)' + re.escape(self.vhost)

                config = textwrap.dedent(f"""
                    # {self.name}

                    var.reg_vhosts = var.reg_vhosts + [ "{vhost_regex}" => {{
                            {errorconfig}
                            accesslog "{accesslog}";
                            {docroot}
                    {self.config}
                        }}
                    ];
                """)
            else:
                config = textwrap.dedent(f"""
                    # {self.name}

                    var.vhosts = var.vhosts + [ "{self.vhost}" => {{
                            {errorconfig}
                            accesslog "{accesslog}";
                            {docroot}
                    {self.config}
                        }}
                    ];
                """)

            self.tests.append_vhosts_config(config)

    def _run(self):
        failed = False
        log(f"[Start] Running test {self.name}")
        try:
            if not self.run_test():
                failed = True
                if self.todo:
                    log(f"Test {self.name} failed")
                else:
                    eprint(f"Test {self.name} failed")
        except Exception as e:
            failed = True
            if self.todo:
                log(f"Test {self.name} failed:")
                log(traceback.format_exc(10))
            else:
                eprint(f"Test {self.name} failed: {e}")
                eprint(traceback.format_exc(10))
                log(f"Test {self.name} failed:")
                log(traceback.format_exc(10))
        log(f"[Done] Running test {self.name} [result={failed and 'Failed' or 'Succeeded'}]")
        self._test_failed = failed and not self.todo
        return not failed

    def _cleanup(self) -> None:
        if self._test_failed and not self._errorlog is None:  # and os.path.exists(self._errorlog):
            with open(self._errorlog) as errors:
                eprint(f"Error log for test {self.name}")
                eprint(errors.read())
        env = self.tests.env
        if env.no_cleanup or (not env.force_cleanup and self._test_failed):
            return
        self.cleanup_test()
        for f in self._test_cleanup_files:
            self._cleanupFile(f)
        for d in self._test_cleanup_dirs:
            self._cleanupDir(d)
        self._test_cleanup_files = []
        self._test_cleanup_dirs = []

    def _cleanupFile(self, fname: str) -> None:
        self.tests.remove_file(fname)

    def _cleanupDir(self, dirname: str) -> None:
        self.tests.remove_dir(dirname)

    # public
    def prepare_vhost_file(self, fname: str, content: str, mode: int = 0o644) -> str:
        """remembers which files have been prepared and removes them on cleanup; returns absolute pathname"""
        fname = os.path.join('www', 'vhosts', self.vhost, fname)
        return self.prepare_file(fname, content, mode=mode)

    def prepare_file(self, fname: str, content: str, mode: int = 0o644) -> str:
        """remembers which files have been prepared and removes them on cleanup; returns absolute pathname"""
        self._test_cleanup_files.append(fname)
        return self.tests.install_file(fname, content, mode=mode)

    def prepare_dir(self, dirname: str) -> str:
        """remembers which directories have been prepared and removes them on cleanup; returns absolute pathname"""
        self._test_cleanup_dirs.append(dirname)
        return self.tests.install_dir(dirname)

    def MissingFeature(self, feature: str) -> bool:
        env = self.tests.env
        eprint(env.COLOR_RED + f"Skipping test {self.name!r} due to missing {feature!r}" + env.COLOR_RESET)
        return False

    # implement these yourself
    def prepare_test(self) -> None:
        pass

    def run_test(self) -> bool:
        raise NotImplementedError()

    def cleanup_test(self) -> None:
        pass


def class2testname(name) -> str:
    if name.startswith("Test"):
        name = name[4:]
    return name


class ModuleTest(TestBase):
    NEVER_REGISTER = True  # only for metaclass
    runnable = False

    _test_list: typing.ClassVar[list[type[TestBase]]]

    def __init_subclass__(cls, *, module: typing.Optional[str] = None, **kwargs):
        super().__init_subclass__(**kwargs)
        # find tests defined in given module (or module class is defined in)
        test_module: str = module or cls.__module__
        cls._test_list = _get_mod_test_list(sys.modules[test_module])
        if not cls.name:
            cls.name = '/' + test_module.rsplit('.', maxsplit=1)[-1].removeprefix('t-') + '/'

    def __init__(self, *, tests: Tests) -> None:
        super().__init__(tests=tests)  # always root test

        self.subtests: list[TestBase] = []
        for c in self._test_list:
            t = c(tests=tests, parent=self)
            self.subtests.append(t)

    def _cleanup(self) -> None:
        for t in self.subtests:
            if t._test_failed:
                self._test_failed = True
        super()._cleanup()


class Tests:
    @staticmethod
    def _fix_test_name(name: str) -> str:
        if not name:
            return '/'
        if (name[:1] != '/'):
            name = '/' + name
        if (name[-1:] != '/'):
            name = name + '/'
        return name

    def __init__(self, *, env: Env) -> None:
        self.env = env
        self.tests_filter = []
        if 0 == len(self.env.tests):
            self.tests_filter.append("")
        else:
            for t in self.env.tests:
                self.tests_filter.append(Tests._fix_test_name(t))

        self.services: list[Service] = []
        self.run: list[TestBase] = []  # tests we want to run
        self.tests: list[TestBase] = []  # all tests (we always prepare/cleanup all tests)
        self.tests_dict: dict[str, TestBase] = {}
        self._ready_for_config = False
        self.config: str = ''
        self.vhosts_config: str = ''
        self.testname_len: int = 60

        self.prepared_dirs: dict[str, int] = {}  # refcounted
        self.prepared_files: set[str] = set()

        self.failed = False

        self.stat_pass = 0
        self.stat_fail = 0
        self.stat_todo = 0
        self.stat_done = 0

        from pylt.service import Lighttpd
        self.add_service(Lighttpd(tests=self))

    def _add_test(self, test: TestBase) -> None:
        name = test.name
        if name in self.tests_dict:
            raise Exception(f"Test {name!r} already defined")
        self.tests_dict[name] = test
        if test.runnable:
            for f in self.tests_filter:
                if name.startswith(f):
                    self.run.append(test)
                    self.testname_len = max(self.testname_len, len(name))
                    break
        self.tests.append(test)

    def add_service(self, service: Service) -> None:
        self.services.append(service)

    def append_config(self, config: str) -> None:
        if not self._ready_for_config:
            raise Exception("Not ready for adding config")
        self.config += config + "\n"

    def append_vhosts_config(self, config: str) -> None:
        if not self._ready_for_config:
            raise Exception("Not ready for adding config")
        self.vhosts_config += config + "\n"

    def load_tests(self) -> None:
        module_names = sorted(
            entry.removesuffix('.py')
            for entry in os.listdir(os.path.join(os.path.dirname(__file__), 'tests'))
            if entry.startswith('t-') and entry.endswith('.py')
        )

        mods: list[types.ModuleType] = []
        for mod_name in module_names:
            mods.append(importlib.import_module(f'pylt.tests.{mod_name}'))

        for mod in mods:
            mod_test_type = getattr(mod, 'Test', None)
            if mod_test_type is None:
                if _get_mod_test_list(mod):
                    mod_test_type = type('Test', (ModuleTest,), {}, module=mod.__name__)
                else:
                    raise Exception(f'No tests in {mod.__file__}')
            assert issubclass(mod_test_type, TestBase)
            mod_test_type(tests=self)  # instance registers itself

    def prepare_tests(self) -> None:
        log("[Start] Preparing tests")
        cache_disk_etag_dir = self.install_dir(os.path.join("tmp", "cache_etag"))
        errorlog = self.install_file("log/error.log", "")
        errorconfig = self.env.debug and " " or f"""log [ default => "file:{errorlog}" ];"""
        accesslog = self.install_file("log/access.log", "")
        self.config = textwrap.dedent(fr"""
            global var.contribdir = "{self.env.contribdir}";
            global var.ssldir = "{self.env.sourcedir}/tests/ca";
            global var.default_docroot = "{self.env.defaultwww}";

            setup {{
                workers 2;

                module_load [
                    "mod_accesslog",
                    "mod_cache_disk_etag",
                    "mod_deflate",
                    "mod_dirlist",
                    "mod_gnutls",
                    "mod_lua",
                    "mod_openssl",
                    "mod_vhost"
                ];

                listen "127.0.0.2:{self.env.port}";
                gnutls [
                    "listen" => "127.0.0.2:" + cast(string)({self.env.port} + 1),
                    "pemfile" => var.ssldir + "/server_test1.ssl.pem",
                    "pemfile" => var.ssldir + "/server_test2.ssl.pem",
                ];
                openssl [
                    "listen" => "127.0.0.2:" + cast(string)({self.env.port} + 2),
                    "pemfile" => var.ssldir + "/server_test1.ssl.pem",
                    "ca-file" => var.ssldir + "/intermediate.crt",
                ];

                log [ default => "stderr" ];

                lua.plugin var.contribdir + "/core.lua";
                lua.plugin var.contribdir + "/secdownload.lua";

                accesslog.format "%h %V %u %t \"%r\" %>s %b \"%{{Referer}}i\" \"%{{User-Agent}}i\"";
                accesslog "{accesslog}";

                debug.log_request_handling true;

                # default values, just check whether they parse
                static.range_requests true;
                keepalive.timeout 5;
                keepalive.requests 0;
                etag.use ["inode", "mtime", "size"];
                stat.async true;
                buffer_request_body true;

                io.timeout 300;
                stat_cache.ttl 10;
            }}

            proxy_protocol.trust;

            {errorconfig}

            include var.contribdir + "/mimetypes.conf";

            global defaultaction = {{
                docroot var.default_docroot;
            }};

            global do_deflate = {{
                if request.is_handled {{
                    deflate;
                }}
            }};

            var.vhosts = [];
            var.reg_vhosts = [];
        """)

        self.vhosts_config = ""
        self._ready_for_config = True

        for t in self.tests:
            log(f"[Start] Preparing test {t.name!r}")
            t._prepare()

        self.config += self.vhosts_config

        self.config += textwrap.dedent(f"""

            var.reg_vhosts = var.reg_vhosts + [ default => {{
                defaultaction;
            }} ];
            var.vhosts = var.vhosts + [ default => {{
                vhost.map_regex var.reg_vhosts;
            }} ];

            vhost.map var.vhosts;

            static;
            do_deflate;

            if request.is_handled {{
                cache.disk.etag "{cache_disk_etag_dir}";
            }}
        """)
        self.env.lighttpdconf = self.install_file("conf/lighttpd.conf", self.config)

        valgrindconfig = ""
        if self.env.valgrind_leak:
            suppressions = ''
            if self.env.valgrind_leak != "":
                suppressions = f', "--suppressions={self.env.valgrind_leak}"'
            valgrindconfig = \
                f'wrapper [ "{self.env.valgrind}", "--leak-check=full", "--show-reachable=yes", ' \
                f'"--leak-resolution=high" {suppressions} ];'
        elif self.env.valgrind:
            valgrindconfig = \
                f'wrapper [ "{self.env.valgrind}" ];'

        gnutlsport = self.env.port + 1
        opensslport = self.env.port + 2
        self.env.angelconf = self.install_file(
            "conf/angel.conf",
            textwrap.dedent(f"""
                binary "{self.env.worker}";
                config "{self.env.lighttpdconf}";
                modules_path "{self.env.plugindir}";
                copy_env [ "PATH" ];
                env [ "G_SLICE=always-malloc", "G_DEBUG=gc-friendly,fatal-criticals,resident-modules" ];
                {valgrindconfig}

                allow_listen "127.0.0.2:{self.env.port}";
                allow_listen ["127.0.0.2:{gnutlsport}", "127.0.0.2:{opensslport}"];
            """),
        )

        log("[Done] Preparing tests")

        log("[Start] Preparing services")
        for s in self.services:
            try:
                s._prepare()
            except BaseException:
                self.failed = True
                raise
        log("[Done] Preparing services")

    @staticmethod
    def _progress(i: int, n: int) -> str:
        s = str(n)
        return f"[{{0:>{len(s)}}}".format(i) + "/" + s + "]"

    def run_tests(self) -> bool:
        env = self.env
        PASS = env.COLOR_GREEN + "[PASS]" + env.COLOR_RESET
        FAIL = env.COLOR_RED + "[FAIL]" + env.COLOR_RESET
        TODO = env.COLOR_YELLOW + "[TODO]" + env.COLOR_RESET
        DONE = env.COLOR_YELLOW + "[DONE]" + env.COLOR_RESET

        testcount = len(self.run)
        log("[Start] Running tests")
        env.log.flush()
        env.stdout.flush()
        env.stderr.flush()

        fmt = env.COLOR_BLUE + f" {{0:<{self.testname_len}}}   "
        failed = False
        i = 1
        for t in self.run:
            result = t._run()
            if t.todo:
                print_stdout(
                    env.COLOR_CYAN + self._progress(i, testcount) + fmt.format(t.name) + (result and DONE or TODO)
                )
                if result:
                    self.stat_done += 1
                else:
                    self.stat_todo += 1
            else:
                print_stdout(
                    env.COLOR_CYAN + self._progress(i, testcount) + fmt.format(t.name) + (result and PASS or FAIL)
                )
                if result:
                    self.stat_pass += 1
                else:
                    self.stat_fail += 1
                    failed = True
            i += 1
            env.log.flush()
            env.stdout.flush()
            env.stderr.flush()
        self.failed = failed
        pass_pct = (100.0 * self.stat_pass)/testcount
        print_stdout(
            f"{self.stat_pass} out of {testcount} tests passed ({pass_pct:.2f}%), {self.stat_fail} tests failed, "
            f"{self.stat_todo} todo items, {self.stat_done} todo items are ready"
        )
        log(f"[Done] Running tests [result={failed and 'Failed' or 'Succeeded'}]")

        env.log.flush()
        env.stdout.flush()
        env.stderr.flush()

        return not failed

    def cleanup_tests(self) -> None:
        # eprint(f"cleanup_files: {self.prepared_files}, cleanup_dirs: {self.prepared_dirs}")

        if not self.env.no_cleanup and not self.failed:
            log("[Start] Cleanup services")
            for s in self.services:
                s._cleanup()
            log("[Done] Cleanup services")
        else:
            log("[Start] Stopping services")
            for s in self.services:
                s._stop()
            log("[Done] Stopping services")

        log("[Start] Cleanup tests")
        for t in self.tests:
            t._cleanup()
        if not self.env.no_cleanup and not self.failed:
            self.remove_file("log/access.log")
            self.remove_file("log/error.log")
            self.remove_file("conf/lighttpd.conf")
            self.remove_file("conf/angel.conf")
            cache_disk_etag_dir = os.path.join(self.env.dir, "tmp", "cache_etag")
            try:
                import shutil
                shutil.rmtree(cache_disk_etag_dir)
                os.mkdir(cache_disk_etag_dir)
            except Exception as e:
                eprint(f"Couldn't clear directory {cache_disk_etag_dir!r}: {e}")
            self.remove_dir(os.path.join("tmp", "cache_etag"))
        log("[Done] Cleanup tests")

    ## helpers for prepare/cleanup

    def _preparefile(self, fname: str, content: str, mode: int = 0o644) -> None:
        if fname in self.prepared_files:
            raise Exception(f"File {fname!r} already exists!")
        else:
            path = os.path.join(self.env.dir, fname)
            f = open(path, "w")
            f.write(content)
            f.close()
            os.chmod(path, mode)
            self.prepared_files.add(fname)

    def _cleanupfile(self, fname: str) -> bool:
        if fname in self.prepared_files:
            try:
                os.remove(os.path.join(self.env.dir, fname))
            except Exception as e:
                eprint(f"Couldn't delete file '{fname}': {e}")
                return False
            return True
        else:
            return False

    def _preparedir(self, dirname: str) -> None:
        if dirname in self.prepared_dirs:
            self.prepared_dirs[dirname] += 1
        else:
            os.mkdir(os.path.join(self.env.dir, dirname))
            self.prepared_dirs[dirname] = 1

    def _cleanupdir(self, dirname: str) -> None:
        self.prepared_dirs[dirname] -= 1
        if 0 == self.prepared_dirs[dirname]:
            try:
                os.rmdir(os.path.join(self.env.dir, dirname))
            except Exception as e:
                eprint(f"Couldn't delete directory {dirname!r}: {e}")

    def install_file(self, fname: str, content: str, mode: int = 0o644) -> str:
        """Add file to tmpdir. Needs call to remove_file later to clean it up."""
        path = list(filter(lambda x: x != '', fname.split('/')))
        for i in range(1, len(path)):
            self._preparedir(os.path.join(*path[0:i]))
        self._preparefile(os.path.join(*path), content, mode=mode)
        return os.path.join(self.env.dir, *path)

    def remove_file(self, fname: str) -> None:
        """Undo install_file on cleanup"""
        path = list(filter(lambda x: x != '', fname.split('/')))
        if not self._cleanupfile(os.path.join(*path)):
            return
        for i in reversed(range(1, len(path))):
            self._cleanupdir(os.path.join(*path[0:i]))

    def install_dir(self, dirname: str) -> str:
        """
        Add dir to tmpdir. Needs symmetrical calls to remove_dir later to clean it up.
        Can be called more than once
        """
        path = list(filter(lambda x: x != '', dirname.split('/')))
        for i in range(1, len(path)+1):
            self._preparedir(os.path.join(*path[0:i]))
        return os.path.join(self.env.dir, *path)

    def remove_dir(self, dirname: str) -> None:
        """Undo install_dir on cleanup"""
        path = list(filter(lambda x: x != '', dirname.split('/')))
        for i in reversed(range(1, len(path)+1)):
            self._cleanupdir(os.path.join(*path[0:i]))
