# -*- coding: utf-8 -*-

import os
import atexit
import subprocess
import socket
import select
import signal

import base

__all__ = [ "Service", "ServiceException" ]

class ServiceException(Exception):
	def __init__(self, value): self.value = value
	def __str__(self): return repr(self.value)

def devnull():
	try:
		f = open("/dev/null", "r")
		return f
	except:
		return None
	

straceargs = [ 'strace', '-tt', '-f', '-s', '4096', '-o' ]

def preexec():
	os.setsid()

class Service(object):
	name = None

	def __init__(self):
		self.proc = None
		self.tests = None
		self.failed = False

	def fork(self, *args):
		if None == self.name:
			raise ServiceException("Service needs a name!")
		log = self.tests.PrepareFile("log/service-%s.log" % self.name, "")
		logfile = open(log, "w")
		inp = devnull()

		if base.Env.strace:
			slog = self.tests.PrepareFile("log/strace-%s.log" % self.name, "")
			args = straceargs + [ slog ] + list(args)

		print >> base.Env.log, "Spawning '%s': %s" % (self.name, ' '.join(args))
		proc = subprocess.Popen(args, stdin = inp, stdout = logfile, stderr = logfile, close_fds = True, preexec_fn = preexec)
		if None != inp: inp.close()
		logfile.close()
		self.proc = proc
		atexit.register(self.kill)

	def kill(self):
		proc = self.proc
		if None == proc: return
		self.proc = None
		if None == proc.poll():
			print >> base.Env.log, "Terminating service '%s'" % (self.name)
			try:
				os.killpg(proc.pid, signal.SIGINT)
				proc.terminate()
			except:
				pass
			print >> base.Env.log, "Waiting for service '%s'" % (self.name)
			proc.wait()

	def portfree(self, port):
		s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		try:
			s.connect(("127.0.0.1", port))
		except:
			pass
		else:
			raise ServiceException("Cannot start service '%s', port 127.0.0.1:%i already in use" % (self.name, port))
		finally:
			s.close()

	def waitconnect(self, port):
		timeout = 5*10
		while True:
			if None != self.proc.poll():
				raise ServiceException("Service %s died before we could establish a connection" % (self.name))
			s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
			try:
				s.connect(("127.0.0.1", port))
			except:
				pass
			else:
				return True
			finally:
				s.close()
			select.select([], [], [], 0.1)
			timeout -= 1
			if 0 > timeout:
				raise ServiceException("Timeout: cannot establish a connection to service %s on port %i" % (self.name, port))

	def _prepare(self):
		self.failed = True
		self.Prepare()
		self.failed = False

	def _cleanup(self):
		self.kill()
		if not base.Env.force_cleanup and self.failed:
			return
		self.tests.CleanupFile("log/service-%s.log" % self.name)
		self.tests.CleanupFile("log/strace-%s.log" % self.name)
		self.Cleanup()

	def _stop(self):
		self.kill()
		self.Stop()

	def Prepare(self):
		raise BaseException("Not implemented yet")

	def Cleanup(self):
		pass

	def Stop(self):
		pass