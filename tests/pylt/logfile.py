# -*- coding: utf-8 -*-

import datetime
import io
import time
import typing


__all__ = ['LogFile', 'RemoveEscapeSeq']


class LogFile(io.TextIOBase):
    __slots__ = ('_file', '_clones', '_newline')

    def __init__(
        self,
        file: typing.Union[io.TextIOBase, typing.IO[str]],
        **clones: typing.Union[io.TextIOBase, typing.IO[str]],
    ):
        super().__init__()
        if file.closed:
            # probably not a very useful state
            super().close()
        self._file = file
        self._clones = clones
        # whether next write should start a new line, i.e. prepend timestamp
        self._newline = True

    def close(self) -> None:
        if not self.closed:
            self.flush()
            super().close()
            self._file.close()

    def writable(self) -> bool:
        return True

    def _write_line_part(self, /, line_part: str) -> None:
        # line_part musn't contain a newline, it only is allowed to end
        # with a newline
        if not line_part:
            return
        if self._newline:
            now = datetime.datetime.now(tz=datetime.timezone(datetime.timedelta(seconds=time.timezone)))
            ts = now.strftime("%Y/%m/%d %H:%M:%S.%f %z: ")
            self._file.write(f"{ts}: {line_part}")
            for (prefix, f) in self._clones.items():
                f.write(f"{ts} {prefix}: {line_part}")
        else:
            self._file.write(line_part)
            for f in self._clones.values():
                f.write(line_part)
        self._newline = line_part.endswith('\n')

    def write(self, /, data: str) -> int:
        lines = data.split('\n')
        for line in lines[:-1]:
            # all but the final line had a terminating '\n' in the input
            self._write_line_part(line + '\n')
        if lines[-1]:
            self._write_line_part(lines[-1])
        return len(data)

    def flush(self) -> None:
        if not self._file.closed:
            self._file.flush()
        for f in self._clones.values():
            if not f.closed:
                f.flush()


class RemoveEscapeSeq(io.TextIOBase):
    __slots__ = ('_file', '_escape_open')

    def __init__(self, file: typing.Union[io.TextIOBase, typing.IO[str]]):
        super().__init__()
        if file.closed:
            super().close()
        self._file = file
        self._escape_open = False

    def close(self) -> None:
        if not self.closed:
            super().close()
            self._file.close()

    def flush(self) -> None:
        self._file.flush()

    def write(self, /, data: str) -> int:
        rem = data
        while rem:
            if self._escape_open:
                parts = str.split('m', maxsplit=1)
                if len(parts) == 2:
                    self._escape_open = False
                    rem = parts[1]
                else:
                    break
            else:
                parts = rem.split('\033', maxsplit=1)
                self._file.write(parts[0])
                if len(parts) == 2:
                    self._escape_open = True
                    rem = parts[1]
                else:
                    break
        return len(data)
