# -*- coding: utf-8 -*-

import time

__all__ = [ 'LogFile' ]

ATTRS = [ 'closed', 'encoding', 'errors', 'mode', 'name', 'newlines', 'softspace' ]

class LogFile(object):
	def __init__(self, file, **clones):
		self.file = file
		self.clones = clones
		self.newline = True

	def __enter__(self, *args, **kwargs): return self.file.__enter__(*args, **kwargs)
	def __exit__(self, *args, **kwargs): return self.file.__exit__(*args, **kwargs)
	def __iter__(self, *args, **kwargs): return self.file.__iter__(*args, **kwargs)
	def __repr__(self, *args, **kwargs): return self.file.__repr__(*args, **kwargs)

	def __delattr__(self, name):
		if name in ATTRS:
			return delattr(self.file, name)
		else:
			return super(LogFile, self).__delattr__(name, value)
	def __getattr__(self, name):
		if name in ATTRS:
			return getattr(self.file, name)
		else:
			return super(LogFile, self).__getattr__(name, value)
	def __getattribute__(self, name):
		if name in ATTRS:
			return self.file.__getattribute__(name)
		else:
			return object.__getattribute__(self, name)
	def __setattr__(self, name, value):
		if name in ATTRS:
			return setattr(self.file, name, value)
		else:
			return super(LogFile, self).__setattr__(name, value)

	def close(self, *args, **kwargs): return self.file.close(*args, **kwargs)
	def fileno(self, *args, **kwargs):
		pass
	def flush(self, *args, **kwargs):
		for (p, f) in self.clones.items():
			f.flush(*args, **kwargs)
		return self.file.flush(*args, **kwargs)
	def isatty(self, *args, **kwargs): return False
	def next(self, *args, **kwargs): return self.file.next(*args, **kwargs)

	def read(self, *args, **kwargs): return self.file.read(*args, **kwargs)
	def readinto(self, *args, **kwargs): return self.file.readinto(*args, **kwargs)
	def readline(self, *args, **kwargs): return self.file.readline(*args, **kwargs)
	def readlines(self, *args, **kwargs): return self.file.readlines(*args, **kwargs)

	def seek(self, *args, **kwargs):
		pass
	def tell(self, *args, **kwargs): return self.file.tell(*args, **kwargs)
	def truncate(self, *args, **kwargs):
		pass
	def __write(self, str):
		self.file.write(str)
		for (p, f) in self.clones.items():
			f.write(p + str)
	def _write(self, str):
		if "" == str: return
		if self.newline:
			# "%f" needs python 2.6
			# ts = time.strftime("%Y/%m/%d %H:%M:%S.%f %Z: ")
			ts = time.strftime("%Y/%m/%d %H:%M:%S %Z")
			self.file.write(ts + ": " + str)
			for (p, f) in self.clones.items():
				f.write(ts + " " + p + ": " + str)
		else:
			self.file.write(str)
			for (p, f) in self.clones.items():
				f.write(str)
		self.newline = ('\n' == str[-1])
	def write(self, str):
		lines = str.split('\n')
		for l in lines[:-1]:
			self._write(l + '\n')
		self._write(lines[-1])
	def writelines(self, *args):
		return self.write(''.join(args))
	def xreadlines(self, *args, **kwargs): return self.file.xreadlines(*args, **kwargs)
