#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
import socket
import time
import random
import traceback
import typing


class MemcacheEntry:
    def __init__(self, flags: bytes, exptime: bytes, data: bytes, cas: bytes):
        self.flags: int = self._flags(flags)
        self.data: bytes = data
        self.cas: bytes = cas
        self.expire: typing.Optional[float] = self._ttl(exptime)

    @staticmethod
    def _flags(flags: bytes) -> int:
        v = int(flags)
        if v < 0 or v >= 2**32:
            raise ValueError('flags not an unsigned 32-bit integer')
        return v

    @staticmethod
    def _ttl(value: bytes) -> typing.Optional[float]:
        v = int(value)
        if v < 0:
            raise ValueError('exptime not an unsigned integer')
        if v > 0 and v < 365*24*3600:
            return v + time.time()
        else:
            return None

    def setExptime(self, exptime: bytes):
        self.expire = self._ttl(exptime)

    def flush(self, exptime: float):
        # make sure entry expires at `exptime` (or before)
        if self.expire is None or self.expire > exptime:
            self.expire = exptime

    def expired(self):
        return not self.expire is None and self.expire < time.time()

    def format_value_line(self, *, key: bytes, include_cas: bool) -> bytes:
        props = f' {self.flags} {len(self.data)}'.encode()
        if include_cas:
            props += b' ' + self.cas
        return b'VALUE ' + key + props + b'\r\n' + self.data + b'\r\n'


class MemcacheDB:
    def __init__(self) -> None:
        self.d: dict[bytes, MemcacheEntry] = {}
        self._cas = random.randint(0, 2**64-1)

    @staticmethod
    def _uint64value(str_value: bytes) -> int:
        v = int(str_value)
        if v < 0 or v >= 2**64:
            raise ValueError('not an unsigned 64-bit integer')
        return v

    def _next_cas(self) -> bytes:
        cas = self._cas
        self._cas = (cas + 1) % 2**64
        return str(cas).encode()

    def get(self, key: bytes) -> typing.Optional[MemcacheEntry]:
        if not key in self.d:
            return None
        entry = self.d[key]
        if entry.expired():
            self.d.pop(key)
            return None
        return entry

    def set(self, key: bytes, flags: bytes, exptime: bytes, data: bytes) -> bytes:
        self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
        return b"STORED"

    def add(self, key: bytes, flags: bytes, exptime: bytes, data: bytes) -> bytes:
        if not self.get(key) is None:
            return b"NOT_STORED"
        self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
        return b"STORED"

    def replace(self, key: bytes, flags: bytes, exptime: bytes, data: bytes) -> bytes:
        if self.get(key) is None:
            return b"NOT_STORED"
        self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
        return b"STORED"

    def append(self, key: bytes, data: bytes) -> bytes:
        entry = self.get(key)
        if entry is None:
            return b"NOT_FOUND"
        entry.data += data
        entry.cas = self._next_cas()
        return b"STORED"

    def prepend(self, key: bytes, data: bytes) -> bytes:
        entry = self.get(key)
        if entry is None:
            return b"NOT_FOUND"
        entry.data = data + entry.data
        entry.cas = self._next_cas()
        return b"STORED"

    def cas(self, key: bytes, flags: bytes, exptime: bytes, cas: bytes, data: bytes) -> bytes:
        entry = self.get(key)
        if entry is None:
            return b"NOT_FOUND"
        if entry.cas != cas:
            return b"EXISTS"
        self.d[key] = MemcacheEntry(flags, exptime, data, self._next_cas())
        return b"STORED"

    def delete(self, key: bytes) -> bytes:
        entry = self.get(key)
        if entry is None:
            return b"NOT_FOUND"
        self.d.pop(key)
        return b"DELETED"

    def incr(self, key: bytes, value: bytes) -> bytes:
        try:
            num_incr = self._uint64value(value)
        except ValueError as e:
            return f"CLIENT_ERROR {e}".encode()
        entry = self.get(key)
        if entry is None:
            return b"NOT_FOUND"
        try:
            num_val = self._uint64value(entry.data)
            num_val = (num_val + num_incr) % 2**64
            entry.data = str(num_val).encode()
            entry.cas = self._next_cas()
        except ValueError as e:
            return f"SERVER_ERROR {e}".encode()
        return entry.data

    def decr(self, key: bytes, value: bytes) -> bytes:
        try:
            num_decr = self._uint64value(value)
        except ValueError as e:
            return f"CLIENT_ERROR {e}".encode()
        entry = self.get(key)
        if entry is None:
            return b"NOT_FOUND"
        try:
            num_val = self._uint64value(entry.data)
            num_val = max(0, num_val - num_decr)
            entry.data = str(num_val).encode()
            entry.cas = self._next_cas()
        except ValueError as e:
            return f"SERVER_ERROR {e}".encode()
        return entry.data

    def touch(self, key: bytes, exptime: bytes) -> bytes:
        entry = self.get(key)
        if entry is None:
            return b"NOT_FOUND"
        entry.setExptime(exptime)
        return b"TOUCHED"

    def stats(self) -> list[tuple[str, int]]:
        return []

    def flush_all(self, exptime: typing.Optional[bytes] = None) -> bytes:
        if exptime is None:
            self.d = {}
        else:
            expire_at = MemcacheEntry._ttl(exptime) or time.time()
            for key in self.d.keys():
                entry = self.get(key)
                if not entry is None:
                    entry.flush(expire_at)
        return b"OK"

    def version(self) -> bytes:
        return b"VERSION python memcached stub 0.1"

    def verbosity(self, level: bytes) -> bytes:
        return b"OK"


class MemcachedHandler:
    def __init__(self, *, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, db: MemcacheDB) -> None:
        self.reader = reader
        self.writer = writer
        self.db = db
        self.data = b''
        self.want_binary: typing.Optional[int] = None
        self.closed = False
        # current command data
        self.cmd: str = ''
        self.args: list[bytes] = []
        self.noreply = False

    def _server_error(self, msg: str) -> None:
        self.writer.write(f'SERVER_ERROR {msg}\r\n'.encode())
        self.data = b''
        self.closed = True

    def _client_error(self, msg: str) -> None:
        self.writer.write(f'CLIENT_ERROR {msg}\r\n'.encode())
        self.data = b''
        self.closed = True

    def _error(self) -> None:
        self.writer.write(b'ERROR\r\n')
        self.data = b''
        self.closed = True

    def _handle_binary(self, b: bytes) -> None:
        cmd, args, noreply = (self.cmd, self.args, self.noreply)
        self.want_binary = None
        (self.cmd, self.args, self.noreply) = ('', [], False)
        r = (getattr(self.db, cmd))(*args, b)
        if not noreply:
            self.writer.write(r + b'\r\n')

    def _handle_line_with_data(self, *, cmd: str, args: list[bytes], noreply: bool) -> None:
        # length of data is always in args[3]; len(args) >= 4
        assert len(args) >= 4
        try:
            want_binary = int(args.pop(3))
        except ValueError as e:
            self._client_error(f"invalid length: {e}")
            return
        if want_binary < 0:
            self._client_error("negative bytes length")
            return
        # wait for data and call cmd handler later through _handle_binary
        self.want_binary = want_binary
        (self.cmd, self.args, self.noreply) = (cmd, args, noreply)

    def _handle_line(self, line: bytes) -> None:
        args = line.split()
        if len(args) == 0:
            self._client_error("empty command")
            return
        cmd = args[0].decode('ascii')
        args = args[1:]
        noreply = False
        if len(args) > 0 and args[-1] == b"noreply":
            args.pop()
            noreply = True
        if cmd in ['set', 'add', 'replace', 'append', 'prepend']:
            if len(args) != 4:
                self._client_error(f"wrong number of arguments {len(args)} for command {cmd}")
                return
            self._handle_line_with_data(cmd=cmd, args=args, noreply=noreply)
        elif cmd == 'cas':
            if len(args) != 5:
                self._client_error(f"wrong number of arguments {len(args)} for command {cmd}")
                return
            self._handle_line_with_data(cmd=cmd, args=args, noreply=noreply)
        elif cmd == 'get':
            for key in args:
                entry = self.db.get(key)
                if not entry is None:
                    self.writer.write(entry.format_value_line(key=key, include_cas=False))
            self.writer.write(b'END\r\n')
        elif cmd == 'gets':
            for key in args:
                entry = self.db.get(key)
                if not entry is None:
                    self.writer.write(entry.format_value_line(key=key, include_cas=True))
            self.writer.write(b'END\r\n')
        elif cmd == 'stats':
            for (name, value) in self.db.stats():
                self.writer.write(f'STAT {name} {value}\r\n'.encode())
            self.writer.write(b'END\r\n')
        elif cmd in ['delete', 'incr', 'decr', 'touch', 'flush_all', 'version', 'verbosity']:
            r = (getattr(self.db, cmd))(*args)
            if not noreply:
                self.writer.write(r + b'\r\n')
        else:
            return self._error()

    def _handle_data(self) -> None:
        while len(self.data) > 0:
            if not self.want_binary is None:
                if len(self.data) >= self.want_binary + 2:
                    b = self.data[:self.want_binary]
                    if self.data[self.want_binary:self.want_binary+2] != b'\r\n':
                        self._client_error("wrong termination of binary data")
                        return
                    self.data = self.data[self.want_binary+2:]
                    self._handle_binary(b)
                else:
                    return  # wait for more data
            else:
                pos = self.data.find(b'\r\n')
                if pos < 0:
                    if len(self.data) > 512:
                        self._client_error("command too long")
                        return
                    return  # wait for more data
                line = self.data[:pos]
                self.data = self.data[pos+2:]
                self._handle_line(line)

    async def handle(self) -> None:
        while not self.closed:
            await self.writer.drain()
            next_buf = await self.reader.read(8192)
            if len(next_buf) == 0:
                # received EOF, close immediately.
                self.writer.close()
                return
            self.data += next_buf
            try:
                self._handle_data()
            except TypeError as e:
                self._client_error(f"wrong number of arguments for command: {e}")
                print(traceback.format_exc())
        # close
        await self.writer.drain()
        self.writer.close()
        await self.writer.wait_closed()


async def main() -> None:
    sock = socket.socket(fileno=0)
    db = MemcacheDB()

    async def handle_memcache_client(
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        print("Memcached: Incoming connection", flush=True)
        await MemcachedHandler(reader=reader, writer=writer, db=db).handle()

    if sock.type == socket.AF_UNIX:
        server = await asyncio.start_unix_server(handle_memcache_client, sock=sock, start_serving=False)
    else:
        server = await asyncio.start_server(handle_memcache_client, sock=sock, start_serving=False)

    addr = server.sockets[0].getsockname()
    print(f'Serving on {addr}', flush=True)

    async with server:
        await server.serve_forever()


try:
    asyncio.run(main())
except KeyboardInterrupt:
    pass
