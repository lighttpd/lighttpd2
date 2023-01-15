# -*- coding: utf-8 -*-

from __future__ import annotations

import asyncio
import dataclasses
import enum
import struct
import typing


class ProtocolException(Exception):
    pass


class Type(enum.IntEnum):
    BEGIN_REQUEST = 1  # web server -> backend
    ABORT_REQUEST = 2  # web server -> backend
    END_REQUEST = 3  # backend -> web server (status)
    PARAMS = 4  # web server -> backend (stream name-value pairs)
    STDIN = 5  # web server -> backend (stream request body)
    STDOUT = 6  # backend -> web server (stream response body)
    STDERR = 7  # backend -> web server (stream error messages)
    DATA = 8  # web server -> backend (stream additional data)
    GET_VALUES = 9  # web server -> backend (request names-value pairs with empty values)
    GET_VALUES_RESULT = 10  # backend -> web server (response name-value pairs)
    UNKNOWN_TYPE = 11


SERVER_TYPES = {
    Type.BEGIN_REQUEST,
    Type.ABORT_REQUEST,
    Type.PARAMS,
    Type.STDIN,
    Type.DATA,
    Type.GET_VALUES,
}
CLIENT_TYPES = {
    Type.END_REQUEST,
    Type.STDOUT,
    Type.STDERR,
    Type.GET_VALUES_RESULT,
    Type.UNKNOWN_TYPE,
}
_NV_PAIR_LONG_LENGTH_BIT = 2**31


class Role(enum.IntEnum):
    RESPONDER = 1
    AUTHORIZER = 2
    FILTER = 3


class ProtocolStatus(enum.IntEnum):
    REQUEST_COMPLETE = 0
    CANT_MPX_CONN = 1
    OVERLOADED = 2
    UNKNOWN_ROLE = 3


@dataclasses.dataclass
class Packet:
    data: bytes
    pckt_type: int
    req_id: int

    def unpack_server_message(self) -> ServerMessage:
        unpack = _UNPACK_SERVER_MESSAGE.get(self.pckt_type, None)
        if not unpack:
            raise ProtocolException(f'Message type not allowed from server {self.pckt_type}')
        return unpack(self.req_id, self.data)


def _pair_encode_len(data: bytes) -> bytes:
    data_len = len(data)
    if data_len >= 128:
        assert data_len < _NV_PAIR_LONG_LENGTH_BIT
        return struct.pack('>I', data_len | _NV_PAIR_LONG_LENGTH_BIT)
    else:
        return struct.pack('>B', data_len)


def _pair_decode_len(payload: bytes) -> typing.Tuple[int, bytes]:
    if len(payload) == 0:
        raise ProtocolException('Unexpected end of data; looking for pair name/value length')
    data_len = payload[0]
    if data_len >= 128:
        if len(payload) < 4:
            raise ProtocolException('Unexpected end of data; looking for pair name/value length')
        data_len, = struct.unpack('>I', payload[:4])
        return (data_len & ~_NV_PAIR_LONG_LENGTH_BIT, payload[4:])
    else:
        return (data_len, payload[1:])


def pack_name_value_pairs(
    pairs: typing.Iterable[typing.Tuple[bytes, bytes]],
) -> bytes:
    payload = b''
    for name, value in pairs:
        payload += _pair_encode_len(name) + _pair_encode_len(value) + name + value
    return payload


def unpack_name_value_pairs(
    payload: bytes,
) -> typing.Generator[typing.Tuple[bytes, bytes], None, None]:
    while payload:
        (name_len, payload) = _pair_decode_len(payload)
        (value_len, payload) = _pair_decode_len(payload)
        if len(payload) < name_len:
            raise ProtocolException('Unexpected end of data; looking for pair name data')
        name, payload = (payload[:name_len], payload[name_len:])
        if len(payload) < value_len:
            raise ProtocolException('Unexpected end of data; looking for pair value data')
        value, payload = (payload[:value_len], payload[value_len:])
        yield name, value


@dataclasses.dataclass
class MsgBeginRequest:
    req_id: int  # must be > 0
    role: Role = Role.RESPONDER
    keep_alive: bool = False

    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid BEGIN_REQUEST request id {self.req_id}')
        flags = 0
        if self.keep_alive:
            flags |= 0x01
        payload = struct.pack('>HBxxxxx', self.role.value, flags)
        return Packet(data=payload, pckt_type=Type.BEGIN_REQUEST.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgBeginRequest:
        if len(payload) != 8:
            raise ProtocolException(f'invalid BEGIN_REQUEST payload {payload!r}')
        if req_id == 0:
            raise ProtocolException(f'invalid BEGIN_REQUEST request id {req_id}')
        role_num, flags = struct.unpack('>HBxxxxx', payload)
        try:
            role = Role(role_num)
        except ValueError:
            raise ProtocolException(f'invalid BEGIN_REQUEST invalid role {role_num}')
        keep_alive = 0 != (flags & 1)
        return MsgBeginRequest(req_id=req_id, role=role, keep_alive=keep_alive)


@dataclasses.dataclass
class MsgAbortRequest:
    req_id: int  # must be > 0

    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid ABORT_REQUEST request id {self.req_id}')
        return Packet(data=b'', pckt_type=Type.ABORT_REQUEST.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgAbortRequest:
        if payload:
            raise ProtocolException('non-empty ABORT_REQUEST')
        return MsgAbortRequest(req_id=req_id)


@dataclasses.dataclass
class MsgEndRequest:
    req_id: int  # must be > 0
    app_status: int = 0
    protocol_status: ProtocolStatus = ProtocolStatus.REQUEST_COMPLETE

    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid END_REQUEST request id {self.req_id}')
        payload = struct.pack('>IBxxx', self.app_status, self.protocol_status.value)
        return Packet(data=payload, pckt_type=Type.END_REQUEST.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgEndRequest:
        if len(payload) != 8:
            raise ProtocolException(f'invalid END_REQUEST payload {payload!r}')
        if req_id == 0:
            raise ProtocolException(f'invalid END_REQUEST request id {req_id}')
        app_status, protocol_status_num = struct.unpack('>IBxxx', payload)
        try:
            protocol_status = ProtocolStatus(protocol_status_num)
        except ValueError:
            raise ProtocolException(f'invalid END_REQUEST invalid protocol status {protocol_status_num}')
        return MsgEndRequest(req_id=req_id, app_status=app_status, protocol_status=protocol_status)


@dataclasses.dataclass
class _MsgStream:
    req_id: int  # must be > 0
    payload: bytes


class MsgParams(_MsgStream):
    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid PARAMS request id {self.req_id}')
        return Packet(data=self.payload, pckt_type=Type.PARAMS.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgParams:
        if req_id == 0:
            raise ProtocolException(f'invalid PARAMS request id {req_id}')
        return MsgParams(req_id=req_id, payload=payload)


class MsgStdin(_MsgStream):
    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid STDIN request id {self.req_id}')
        return Packet(data=self.payload, pckt_type=Type.STDIN.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgStdin:
        if req_id == 0:
            raise ProtocolException(f'invalid STDIN request id {req_id}')
        return MsgStdin(req_id=req_id, payload=payload)


class MsgStdout(_MsgStream):
    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid STDOUT request id {self.req_id}')
        return Packet(data=self.payload, pckt_type=Type.STDOUT.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgStdout:
        if req_id == 0:
            raise ProtocolException(f'invalid STDOUT request id {req_id}')
        return MsgStdout(req_id=req_id, payload=payload)


class MsgStderr(_MsgStream):
    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid STDERR request id {self.req_id}')
        return Packet(data=self.payload, pckt_type=Type.STDERR.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgStderr:
        if req_id == 0:
            raise ProtocolException(f'invalid STDERR request id {req_id}')
        return MsgStderr(req_id=req_id, payload=payload)


class MsgData(_MsgStream):
    def pack(self) -> Packet:
        if self.req_id == 0:
            raise ProtocolException(f'invalid DATA request id {self.req_id}')
        return Packet(data=self.payload, pckt_type=Type.DATA.value, req_id=self.req_id)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgData:
        if req_id == 0:
            raise ProtocolException(f'invalid DATA request id {req_id}')
        return MsgData(req_id=req_id, payload=payload)


@dataclasses.dataclass
class MsgGetValues:
    names: set[bytes]

    def pack(self) -> Packet:
        payload = pack_name_value_pairs((name, b'') for name in self.names)
        return Packet(data=payload, pckt_type=Type.GET_VALUES.value, req_id=0)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgGetValues:
        if req_id != 0:
            raise ProtocolException(f'invalid GET_VALUES request id {req_id}')
        names: set[bytes] = set()
        for name, value in unpack_name_value_pairs(payload):
            if value:
                raise ProtocolException(f'non-empty value {value!r} in GET_VALUES pair for name {name!r}')
            names.add(name)
        return MsgGetValues(names=names)


@dataclasses.dataclass
class MsgGetValuesResult:
    values: dict[bytes, bytes]

    def pack(self) -> Packet:
        payload = pack_name_value_pairs(self.values.items())
        return Packet(data=payload, pckt_type=Type.GET_VALUES_RESULT.value, req_id=0)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgGetValuesResult:
        if req_id != 0:
            raise ProtocolException(f'invalid GET_VALUES_RESULT request id {req_id}')
        values = dict(unpack_name_value_pairs(payload))
        return MsgGetValuesResult(values=values)


@dataclasses.dataclass
class MsgUnknownType:
    pckt_type: int

    def pack(self) -> Packet:
        payload = struct.pack('>Bxxxxxxx', self.pckt_type)
        return Packet(data=payload, pckt_type=Type.UNKNOWN_TYPE.value, req_id=0)

    @staticmethod
    def unpack(req_id: int, payload: bytes) -> MsgUnknownType:
        if len(payload) != 8:
            raise ProtocolException(f'invalid UNKNOWN_TYPE payload {payload!r}')
        if req_id != 0:
            raise ProtocolException(f'invalid UNKNOWN_TYPE request id {req_id}')
        pckt_type, = struct.unpack('>Bxxxxxxx', payload)
        return MsgUnknownType(pckt_type=pckt_type)


ServerRequestMessage = typing.Union[
    MsgBeginRequest,
    MsgAbortRequest,
    MsgParams,
    MsgStdin,
    MsgData,
]
ServerManagementMessage = MsgGetValues  # just one for now
ServerMessage = typing.Union[ServerRequestMessage, ServerManagementMessage]
ClientRequestMessage = typing.Union[
    MsgEndRequest,
    MsgStdout,
    MsgStderr,
]
ClientManagementMessage = typing.Union[
    MsgGetValuesResult,
    MsgUnknownType,
]
ClientMessage = typing.Union[ClientRequestMessage, ClientManagementMessage]
_UNPACK_SERVER_MESSAGE: dict[int, typing.Callable[[int, bytes], ServerMessage]] = {
    Type.BEGIN_REQUEST.value: MsgBeginRequest.unpack,
    Type.ABORT_REQUEST.value: MsgAbortRequest.unpack,
    Type.PARAMS.value: MsgParams.unpack,
    Type.STDIN.value: MsgStdin.unpack,
    Type.DATA.value: MsgData.unpack,
    Type.GET_VALUES.value: MsgGetValues.unpack,
}
_UNPACK_CLIENT_MESSAGE: dict[int, typing.Callable[[int, bytes], ClientMessage]] = {
    Type.END_REQUEST.value: MsgEndRequest.unpack,
    Type.STDOUT.value: MsgStdout.unpack,
    Type.STDERR.value: MsgStderr.unpack,
    Type.GET_VALUES_RESULT.value: MsgGetValuesResult.unpack,
    Type.UNKNOWN_TYPE.value: MsgUnknownType.unpack,
}


class LowLevelConnection:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._reader = reader
        self._writer = writer
        self._closed = False
        self._write_lock = asyncio.Lock()

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self._writer.close()

    def _abort(self, msg: str) -> typing.NoReturn:
        self.close()
        raise ProtocolException(msg)

    def _error_close(self) -> typing.NoReturn:
        self.close()
        raise EOFError("FastCGI connection is closed")

    async def read(self) -> Packet:
        if self._closed:
            self._error_close()

        try:
            hdr = await self._reader.readexactly(8)
        except asyncio.IncompleteReadError as e:
            if e.partial:
                self._abort("Incomplete header")
            self._error_close()
        except (EOFError, ConnectionError):
            self._error_close()

        version, pckt_type, req_id, data_len, pad_len = struct.unpack('>BBHHBx', hdr)
        if version != 1:
            self._abort(f"Invalid packet version {version}")
        if pad_len >= 8 or (data_len + pad_len) % 8 != 0:
            self._abort(f"Invalid packet pad_len (data_len = {data_len}, pad_len = {pad_len})")
        total = data_len + pad_len
        try:
            data = await self._reader.readexactly(total)
        except EOFError:
            self._abort("Incomplete data")
        return Packet(data=data[:data_len], pckt_type=pckt_type, req_id=req_id)

    async def write(self, packet: Packet):
        async with self._write_lock:
            if self._closed:
                self._error_close()
            version = 1
            data_len = len(packet.data)
            pad_len = (8 - (data_len % 8)) % 8
            padding = b'       '[:pad_len]
            hdr = struct.pack('>BBHHBx', version, packet.pckt_type, packet.req_id, data_len, pad_len)
            self._writer.writelines((hdr, packet.data, padding))
            try:
                await self._writer.drain()
            except ConnectionError as e:
                self._abort(f"Connection reset: {e}")


ApplicationRequestT = typing.TypeVar('ApplicationRequestT', bound='ApplicationRequest')


class Application(typing.Generic[ApplicationRequestT]):
    def __init__(
        self,
        *,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        request_cls: type[ApplicationRequestT],
    ) -> None:
        self._lower = LowLevelConnection(reader=reader, writer=writer)
        self._request_cls = request_cls
        self._requests: dict[int, ApplicationRequestT] = {}
        self._msg_handlers = {
            Type.BEGIN_REQUEST.value: self._handle_begin_request,
            Type.ABORT_REQUEST.value: self._handle_abort_request,
            Type.PARAMS.value: self._handle_params,
            Type.STDIN.value: self._handle_stdin,
            Type.DATA.value: self._handle_data,
            Type.GET_VALUES.value: self._handle_get_values,
        }
        self._values: dict[bytes, bytes] = {}

    def _close(self) -> None:
        self._lower.close()
        requests = list(self._requests.values())
        self._requests.clear()
        for req in requests:
            req._app = None
        for req in requests:
            req._recv_task.cancel()

    async def _handle_begin_request(self, packet: Packet) -> None:
        msg = MsgBeginRequest.unpack(packet.req_id, packet.data)
        if msg.req_id in self._requests:
            raise ProtocolException(f'Request id {msg.req_id} still in use')
        self._requests[msg.req_id] = req = self._request_cls(
            req_id=msg.req_id,
            keep_alive=msg.keep_alive,
        )
        req._app = self

    async def _handle_abort_request(self, packet: Packet) -> None:
        msg = MsgAbortRequest.unpack(packet.req_id, packet.data)
        req = self._requests.get(msg.req_id, None)
        if req:
            # protocol spec: ignore messages for unknown request ids
            await req._recv_queue.put(msg)

    async def _handle_params(self, packet: Packet) -> None:
        msg = MsgParams.unpack(packet.req_id, packet.data)
        req = self._requests.get(msg.req_id, None)
        if req:
            # protocol spec: ignore messages for unknown request ids
            await req._recv_queue.put(msg)

    async def _handle_stdin(self, packet: Packet) -> None:
        msg = MsgStdin.unpack(packet.req_id, packet.data)
        req = self._requests.get(msg.req_id, None)
        if req:
            # protocol spec: ignore messages for unknown request ids
            await req._recv_queue.put(msg)

    async def _handle_data(self, packet: Packet) -> None:
        msg = MsgData.unpack(packet.req_id, packet.data)
        req = self._requests.get(msg.req_id, None)
        if req:
            # protocol spec: ignore messages for unknown request ids
            await req._recv_queue.put(msg)

    async def _handle_get_values(self, packet: Packet) -> None:
        msg = MsgGetValues.unpack(packet.req_id, packet.data)
        values: dict[bytes, bytes] = {}
        for name in msg.names:
            value = self._values.get(name, None)
            if not value is None:
                values[name] = value
        await self._write(MsgGetValuesResult(values=values))

    async def _write(self, msg: ClientMessage) -> None:
        await self._lower.write(msg.pack())

    async def run(self) -> None:
        try:
            while True:
                try:
                    packet = await self._lower.read()
                except (ProtocolException, EOFError):
                    return

                try:
                    Type(packet.pckt_type)
                except ValueError:
                    await self._write(MsgUnknownType(pckt_type=packet.pckt_type))
                    continue

                handler = self._msg_handlers.get(packet.pckt_type, None)
                if not handler:
                    return

                await handler(packet)
        finally:
            self._close()


class ApplicationRequest:
    def __init__(self, *, req_id: int, keep_alive: bool) -> None:
        self._app: typing.Optional[Application] = None
        self.req_id = req_id
        self.keep_alive = keep_alive
        self.params: dict[bytes, bytes] = {}
        self._params_buf = b''
        self.params_complete = False
        self.stdin_closed = False
        self.data_closed = False
        self.stdout_closed = False
        self.stderr_closed = False
        self.aborted = False
        self.completed = False
        self._recv_queue: asyncio.Queue[typing.Union[
            MsgAbortRequest,
            MsgParams,
            MsgStdin,
            MsgData,
        ]] = asyncio.Queue(maxsize=16)
        self._recv_task = asyncio.Task(self._start())

    async def _start(self) -> None:
        try:
            await self.run()
        finally:
            if not self.completed:
                await self._handle_abort()

    async def run(self) -> None:
        while True:
            item = await self._recv_queue.get()
            if isinstance(item, MsgAbortRequest):
                await self._handle_abort()
            elif isinstance(item, MsgParams):
                await self._recv_params_stream(item.payload)
            elif isinstance(item, MsgStdin):
                await self._recv_stdin_stream(item.payload)
            # elif isinstance(item, MsgData):
            else:
                await self._recv_data_stream(item.payload)

    async def exit(
        self,
        *,
        status: int = 0,
        protocol_status: ProtocolStatus = ProtocolStatus.REQUEST_COMPLETE,
    ) -> None:
        self.stdout_closed = self.stderr_closed = True
        if self._app:
            app = self._app
            self._app = None
            del app._requests[self.req_id]
            await app._write(
                MsgEndRequest(req_id=self.req_id, app_status=status, protocol_status=protocol_status),
            )
        self._recv_task.cancel()
        while True:
            try:
                self._recv_queue.get_nowait()
            except asyncio.QueueEmpty:
                break

    async def finish(
        self,
        *,
        status: int = 0,
        protocol_status: ProtocolStatus = ProtocolStatus.REQUEST_COMPLETE,
    ) -> None:
        await self.write_stdout(b'')
        await self.write_stderr(b'')
        self.completed = True
        await self.exit(status=status, protocol_status=protocol_status)

    async def write_stdout(self, data: bytes, *, no_close: bool = False):
        if self.stdout_closed:
            if len(data) != 0:
                raise Exception('stdout already closed')
        if len(data) == 0:
            if not no_close:
                # sending empty data signals EOF by default
                self.stdout_closed = True
            else:
                return  # don't send the EOF message!
        if self._app:
            await self._app._write(MsgStdout(req_id=self.req_id, payload=data))

    async def write_stderr(self, data: bytes, *, no_close: bool = False):
        if self.stderr_closed:
            if len(data) != 0:
                raise Exception('stderr already closed')
        if len(data) == 0:
            if not no_close:
                # sending empty data signals EOF by default
                self.stderr_closed = True
            else:
                return  # don't send the EOF message!
        if self._app:
            await self._app._write(MsgStderr(req_id=self.req_id, payload=data))

    async def _handle_abort(self) -> None:
        if self.completed:
            return
        if not self.aborted:
            self.aborted = True
            await self.handle_abort()

    async def handle_abort(self) -> None:
        await self.exit(status=-1)

    async def _recv_params_stream(self, data: bytes) -> None:
        if self.params_complete:
            raise ProtocolException('Params already closed')
        if len(data) == 0:
            self.params_complete = True
            self.params.update(unpack_name_value_pairs(self._params_buf))
            self._params_buf = b''
            await self.received_params()
        else:
            self._params_buf += data

    async def received_params(self) -> None:
        pass

    async def _recv_stdin_stream(self, data: bytes) -> None:
        if not self.params_complete:
            raise ProtocolException('Received stdin data before params finished')
        if len(data) == 0:
            self._stdin_closed = True
        await self.recv_stdin(data)

    async def recv_stdin(self, data: bytes) -> None:
        # ignored by default
        pass

    async def _recv_data_stream(self, data: bytes) -> None:
        if len(data) == 0:
            self._data_closed = True
        await self.recv_data(data)

    async def recv_data(self, data: bytes) -> None:
        # ignored by default
        pass
