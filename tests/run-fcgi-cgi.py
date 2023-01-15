#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
import os
import socket
import sys
import typing

sys.path.append(os.path.dirname(__file__))

import pylt.fastcgi


class TestAppRequest(pylt.fastcgi.ApplicationRequest):
    def __init__(self, *, req_id: int, keep_alive: bool) -> None:
        super().__init__(req_id=req_id, keep_alive=keep_alive)
        self._process: typing.Optional[asyncio.subprocess.Process] = None
        self._task_stdout: typing.Optional[asyncio.Task] = None
        self._task_stderr: typing.Optional[asyncio.Task] = None
        self._task_wait: typing.Optional[asyncio.Task] = None

    async def received_params(self) -> None:
        target = self.params.get(b'INTERPRETER') or self.params.get(b'SCRIPT_FILENAME')
        if not target:
            await self.write_stdout(b"Status: 500\r\nContent-Type: text/plain\r\n\r\n")
            await self.finish()
            return
        env = dict(self.params)
        env[b'PATH'] = os.getenvb(b'PATH', b'')
        self._process = process = await asyncio.create_subprocess_exec(
            target,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            env=env,
        )
        self._task_stdout = asyncio.Task(self._handle_task_stdout(process))
        self._task_stderr = asyncio.Task(self._handle_task_stderr(process))
        self._task_wait = asyncio.Task(self._handle_task_wait(process))

    async def recv_stdin(self, data: bytes) -> None:
        if not self._process or not self._process.stdin:
            return
        try:
            if data:
                self._process.stdin.write(data)
                await self._process.stdin.drain()
            else:
                self._process.stdin.close()
                await self._process.stdin.wait_closed()
        except IOError:
            stdin = self._process.stdin
            self._process.stdin = None
            try:
                stdin.close()
            except IOError:
                pass

    async def _handle_task_stdout(self, process: asyncio.subprocess.Process):
        stdout = process.stdout
        try:
            if not stdout:
                return
            while True:
                try:
                    chunk = await stdout.read(8192)
                    if not chunk:
                        return
                    await self.write_stdout(chunk)
                except EOFError:
                    return
        finally:
            await self.write_stdout(b'')

    async def _handle_task_stderr(self, process: asyncio.subprocess.Process):
        stderr = process.stderr
        try:
            if not stderr:
                return
            while True:
                try:
                    chunk = await stderr.read(8192)
                    if not chunk:
                        return
                    await self.write_stderr(chunk)
                except EOFError:
                    return
        finally:
            await self.write_stderr(b'')

    async def _handle_task_wait(self, process: asyncio.subprocess.Process):
        status = await process.wait()
        assert self._task_stdout
        await self._task_stdout
        assert self._task_stderr
        await self._task_stderr
        self._process = None
        await self.finish(status=status)


async def handle_fcgi_cgi(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    print("fcgi-cgi: Incoming connection", flush=True)
    app = pylt.fastcgi.Application(reader=reader, writer=writer, request_cls=TestAppRequest)
    await app.run()


async def main() -> None:
    sock = socket.socket(fileno=0)

    if sock.type == socket.AF_UNIX:
        server = await asyncio.start_unix_server(handle_fcgi_cgi, sock=sock, start_serving=False)
    else:
        server = await asyncio.start_server(handle_fcgi_cgi, sock=sock, start_serving=False)

    addr = server.sockets[0].getsockname()
    print(f'Serving on {addr}', flush=True)

    async with server:
        await server.serve_forever()


try:
    asyncio.run(main())
except KeyboardInterrupt:
    pass
