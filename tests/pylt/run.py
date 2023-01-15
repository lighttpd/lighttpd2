# -*- coding: utf-8 -*-

import os
import sys
from tempfile import mkdtemp
import optparse
import typing

from .logfile import LogFile, RemoveEscapeSeq
from .base import Env, Tests, eprint


def which(program: str) -> typing.Optional[str]:
    def is_exe(fpath: str) -> bool:
        return os.path.exists(fpath) and os.access(fpath, os.X_OK)

    fpath, fname = os.path.split(program)
    if fpath:
        if is_exe(program):
            return program
    else:
        for path in os.environ["PATH"].split(os.pathsep):
            exe_file = os.path.join(path, program)
            if is_exe(exe_file):
                return exe_file

    return None


def find_port(port: int) -> int:
    if port >= 1024 and port < (65536-8):
        return port

    from random import Random
    r = Random(os.getpid())
    return r.randint(1024, 65536-8)


class ArgumentError(Exception):
    pass


def parse_args() -> optparse.Values:
    parser = optparse.OptionParser()
    parser.add_option(
        "--angel",
        help="Path to angel binary (required)",
    )
    parser.add_option(
        "--worker",
        help="Path to worker binary (required)",
    )
    parser.add_option(
        "--plugindir",
        help="Path to plugin directory (required)",
    )
    parser.add_option(
        "-k", "--no-cleanup",
        help="Keep temporary files, no cleanup",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "-p", "--port",
        help="Use [port,port+7] as tcp ports on 127.0.0.2 (default: 8088; use 0 for random port)",
        default=8088,
        type="int",
    )
    parser.add_option(
        "-t", "--test",
        help="Run specific test",
        action="append",
        dest="tests",
        default=[],
    )
    parser.add_option(
        "-c", "--force-cleanup",
        help="Keep no temporary files (overwrites -k)",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--strace",
        help="Strace services",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--truss",
        help="Truss services",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--debug-requests",
        help="Dump requests",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--no-angel",
        help="Spawn lighttpd worker directly",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--debug",
        help="Show service logs on console",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--wait",
        help="Wait for services to exit on first signal",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--valgrind",
        help="Run worker with valgrind from angel",
        action="store_true",
        default=False,
    )
    parser.add_option(
        "--valgrind-leak",
        help="Run valgrind with memory leak check; takes an empty string or a valgrind suppression file",
        action="store",
        default=False,
    )

    (options, args) = parser.parse_args()

    if not options.angel or not options.worker or not options.plugindir:
        raise ArgumentError("Missing required arguments")

    if options.force_cleanup:
        options.no_cleanup = False

    return options


def setup_env() -> Env:
    options = parse_args()

    env = Env()
    env.angel = os.path.abspath(options.angel)
    env.worker = os.path.abspath(options.worker)
    env.plugindir = os.path.abspath(options.plugindir)
    env.no_cleanup = options.no_cleanup
    env.force_cleanup = options.force_cleanup
    env.port = find_port(options.port)
    env.tests = options.tests
    env.sourcedir = os.path.dirname(os.path.dirname(os.path.abspath(os.path.dirname(__file__))))
    env.contribdir = os.path.join(env.sourcedir, "contrib")
    env.debugRequests = options.debug_requests
    env.strace = options.strace
    env.truss = options.truss
    env.no_angel = options.no_angel
    env.debug = options.debug
    env.wait = options.wait
    if options.valgrind or env.valgrind_leak:
        valgrind = which('valgrind')
        if valgrind is None:
            raise ArgumentError("Can't find valgrind binary in path")
        env.valgrind = valgrind
    env.valgrind_leak = options.valgrind_leak

    env.color = sys.stdin.isatty()
    env.COLOR_RESET = env.color and "\033[0m" or ""
    env.COLOR_BLUE = env.color and "\033[1;34m" or ""
    env.COLOR_GREEN = env.color and "\033[1;32m" or ""
    env.COLOR_YELLOW = env.color and "\033[1;33m" or ""
    env.COLOR_RED = env.color and "\033[1;31m" or ""
    env.COLOR_CYAN = env.color and "\033[1;36m" or ""

    env.dir = mkdtemp(suffix='-l2-tests')
    env.defaultwww = os.path.join(env.dir, "www", "default")

    env.log = open(os.path.join(env.dir, "tests.log"), "w")
    if env.color:
        env.log = RemoveEscapeSeq(env.log)
    if env.debug:
        logfile = env.log
        env.log = LogFile(sys.stdout, **{"[log]": logfile})
        env.stdout = LogFile(sys.stdout, **{"[stdout]": logfile})
        env.stderr = LogFile(sys.stderr, **{"[stderr]": logfile})
    else:
        env.log = LogFile(env.log)
        env.stdout = LogFile(sys.stdout, **{"[stdout]": env.log})
        env.stderr = LogFile(sys.stderr, **{"[stderr]": env.log})
    return env


def run_tests_in_env(env: Env) -> bool:
    with env.use():
        # run tests
        tests = Tests(env=env)
        tests.load_tests()
        try:
            tests.prepare_tests()
            result = tests.run_tests()
            return result
        finally:
            tests.cleanup_tests()
            if not env.no_cleanup and result:
                os.remove(os.path.join(env.dir, "tests.log"))


def main() -> None:
    env = setup_env()
    try:
        result = run_tests_in_env(env)
    except Exception:
        import traceback
        eprint(traceback.format_exc())
        result = False
    finally:
        try:
            if env.force_cleanup:
                import shutil
                shutil.rmtree(env.dir)
            elif not env.no_cleanup and result:
                os.rmdir(env.dir)
        except OSError:
            eprint(f"Couldn't delete temporary directory {env.dir!r}, probably not empty (perhaps due to some errors)")
            result = False

    if result:
        sys.exit(0)
    else:
        sys.exit(1)
