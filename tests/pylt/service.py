# -*- coding: utf-8 -*-

import os
import atexit
import shlex
import subprocess
import socket
import select
import signal
import time
import typing

from pylt import base

__all__ = ["Service", "ServiceException", "devnull", "Lighttpd", "FastCGI"]


class ServiceException(Exception):
    pass


def devnull() -> typing.Optional[typing.BinaryIO]:
    try:
        f = open("/dev/null", "rb")
        return f
    except Exception:
        return None


straceargs = ['strace', '-tt', '-f', '-s', '4096', '-o']
trussargs = ['truss', '-d', '-f', '-s', '4096', '-o']


def preexec() -> None:
    os.setsid()


def procwait(proc: subprocess.Popen, timeout: int = 2) -> bool:
    ts = time.time()
    while True:
        if not proc.poll() is None:
            return True
        seconds_passed = time.time() - ts
        if seconds_passed > timeout:
            return False
        time.sleep(0.1)


class FileWithFd(typing.Protocol):
    def fileno(self) -> int:
        ...

    def close(self) -> None:
        ...


class Service:
    name: typing.ClassVar[str]

    def __init__(self, *, tests: base.Tests) -> None:
        self.tests = tests
        self.proc: typing.Optional[subprocess.Popen] = None
        self.failed = False

    def bind_unix_socket(self, *, sockfile: str, backlog: int = 8) -> socket.socket:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        rel_sockfile = os.path.relpath(sockfile)
        if len(rel_sockfile) < len(sockfile):
            sockfile = rel_sockfile
        sock.bind(sockfile)
        sock.listen(8)
        return sock

    def fork(self, *args: str, inp: typing.Union[tuple[()], FileWithFd, None] = ()) -> None:
        arguments = list(args)  # convert tuple to list
        stdin: typing.Optional[FileWithFd]
        if inp == ():
            stdin = devnull()
        else:
            # mypy doesn't get it, but this should be checkable
            stdin = typing.cast(typing.Optional[FileWithFd], inp)
        stdin_fileno: typing.Optional[int]
        if stdin is None:
            stdin_fileno = None
        else:
            stdin_fileno = stdin.fileno()

        if not self.name:
            raise ServiceException("Service needs a name!")
        logfile: typing.Optional[typing.IO]
        if self.tests.env.debug:
            logfile = None
        else:
            logfile = open(self.log, "wb")

        if self.tests.env.strace:
            slog = self.tests.install_file(f"log/strace-{self.name}.log", "")
            arguments = straceargs + [slog] + arguments
        elif self.tests.env.truss:
            tlog = self.tests.install_file(f"log/truss-{self.name}.log", "")
            arguments = trussargs + [tlog] + arguments

        base.log(f"Spawning {self.name!r}: {shlex.join(arguments)}")
        proc = subprocess.Popen(
            arguments,
            stdin=stdin_fileno,
            stdout=logfile,
            stderr=logfile,
            close_fds=True,
            preexec_fn=preexec,
        )
        if not stdin is None:
            stdin.close()
        if not logfile is None:
            logfile.close()
        self.proc = proc
        atexit.register(self.kill)

    def kill(self) -> None:
        s = signal.SIGINT
        ss = "SIGINT"
        proc = self.proc
        if proc is None:
            return
        self.proc = None
        if proc.poll() is None:
            base.log(f"Terminating service ({ss}) {self.name!r}")
            try:
                os.killpg(proc.pid, s)
                s = signal.SIGTERM
                ss = "SIGTERM"
                proc.terminate()
            except Exception:
                pass
            base.log(f"Waiting for service {self.name!r}")
            if self.tests.env.wait:
                proc.wait()
            while not procwait(proc):
                try:
                    base.log(f"Terminating service ({ss}) {self.name!r}")
                    os.killpg(proc.pid, s)
                    s = signal.SIGKILL
                    ss = "SIGKILL"
                    proc.terminate()
                except Exception:
                    pass

    def portfree(self, port: int) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.connect(("127.0.0.2", port))
        except Exception:
            pass
        else:
            raise ServiceException(f"Cannot start service {self.name!r}, port 127.0.0.2:{port} already in use")
        finally:
            s.close()

    def waitconnect(self, port: int) -> bool:
        timeout = 5*10
        while True:
            assert self.proc
            if not self.proc.poll() is None:
                raise ServiceException(f"Service {self.name!r} died before we could establish a connection")
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.connect(("127.0.0.2", port))
            except Exception:
                pass
            else:
                return True
            finally:
                s.close()
            select.select([], [], [], 0.1)
            timeout -= 1
            if 0 > timeout:
                raise ServiceException(
                    f"Timeout: cannot establish a connection to service {self.name!r} on port {port}"
                )

    def _prepare(self) -> None:
        assert self.tests
        self.log = self.tests.install_file(f"log/service-{self.name}.log", "")
        self.failed = True
        self.prepare_service()
        self.failed = False

    def _cleanup(self) -> None:
        self.kill()
        if not self.tests.env.force_cleanup and self.failed:
            return
        assert self.tests
        self.tests.remove_file(f"log/service-{self.name}.log")
        self.tests.remove_file(f"log/strace-{self.name}.log")
        self.tests.remove_file(f"log/truss-{self.name}.log")
        self.cleanup_service()

    def _stop(self) -> None:
        self.kill()
        self.stop_service()

    def prepare_service(self) -> None:
        raise NotImplementedError()

    def cleanup_service(self) -> None:
        pass

    def stop_service(self) -> None:
        pass


class Lighttpd(Service):
    name = "lighttpd"

    def test_config(self) -> None:
        env = self.tests.env
        logfile = open(self.log, "w")
        inp = devnull()
        args = [env.worker, '-m', env.plugindir, '-c', env.lighttpdconf, '-t']
        if env.valgrind:
            args = [env.valgrind] + args
        base.log(f"Testing lighttpd config: {shlex.join(args)}")
        proc = subprocess.Popen(args, stdin=inp, stdout=logfile, stderr=logfile, close_fds=True)
        if not inp is None:
            inp.close()
        logfile.close()
        status = proc.wait()
        if 0 != status:
            subprocess.run(['cat', self.log])
            raise Exception(f"testing lighttpd config failed with returncode {status}")

    def prepare_service(self) -> None:
        env = self.tests.env
        self.test_config()

        self.portfree(env.port)
        if env.no_angel:
            if env.valgrind:
                self.fork(env.valgrind, env.worker, '-m', env.plugindir, '-c', env.lighttpdconf)
            else:
                self.fork(env.worker, '-m', env.plugindir, '-c', env.lighttpdconf)
        else:
            self.fork(env.angel, '-o', '-m', env.plugindir, '-c', env.angelconf)
        self.waitconnect(env.port)


class FastCGI(Service):
    binary: list[str] = []

    def __init__(self, *, tests: base.Tests) -> None:
        super().__init__(tests=tests)
        self.sockfile = os.path.join(self.tests.env.dir, "tmp", "sockets", self.name + ".sock")

    def prepare_service(self) -> None:
        assert self.tests
        self.tests.install_dir(os.path.join("tmp", "sockets"))
        sock = self.bind_unix_socket(sockfile=self.sockfile)
        self.fork(*self.binary, inp=sock)

    def cleanup_service(self) -> None:
        assert self.tests
        try:
            os.remove(self.sockfile)
        except Exception as e:
            base.eprint(f"Couldn't delete socket {self.sockfile!r}: {e}")
        self.tests.remove_dir(os.path.join("tmp", "sockets"))
