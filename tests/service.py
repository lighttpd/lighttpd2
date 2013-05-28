# -*- coding: utf-8 -*-

import os
import atexit
import subprocess
import socket
import select
import signal
import time

import base

__all__ = [ "Service", "ServiceException", "devnull", "Lighttpd", "FastCGI" ]

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
trussargs = [ 'truss', '-d', '-f', '-s', '4096', '-o' ]

def preexec():
	os.setsid()

def procwait(proc, timeout = 2):
	ts = time.time()
	while True:
		if proc.poll() is not None: return True
		seconds_passed = time.time() - ts
		if seconds_passed > timeout:
			return False
		time.sleep(0.1)


class Service(object):
	name = None

	def __init__(self):
		self.proc = None
		self.tests = None
		self.failed = False

	def devnull(self):
		return devnull()

	def fork(self, *args, **kwargs):
		if kwargs.has_key('inp'):
			inp = kwargs['inp']
		else:
			inp = devnull()

		if None == self.name:
			raise ServiceException("Service needs a name!")
		if base.Env.debug:
			logfile = None
		else:
			logfile = open(self.log, "w")

		if base.Env.strace:
			slog = self.tests.PrepareFile("log/strace-%s.log" % self.name, "")
			args = straceargs + [ slog ] + list(args)
		elif base.Env.truss:
			tlog = self.tests.PrepareFile("log/truss-%s.log" % self.name, "")
			args = trussargs + [ tlog ] + list(args)

		print >> base.Env.log, "Spawning '%s': %s" % (self.name, ' '.join(args))
		proc = subprocess.Popen(args, stdin = inp, stdout = logfile, stderr = logfile, close_fds = True, preexec_fn = preexec)
		if None != inp: inp.close()
		if None != logfile: logfile.close()
		self.proc = proc
		atexit.register(self.kill)

	def kill(self):
		s = signal.SIGINT
		ss = "SIGINT"
		proc = self.proc
		if None == proc: return
		self.proc = None
		if None == proc.poll():
			print >> base.Env.log, "Terminating service (%s) '%s'" % (ss, self.name)
			try:
				os.killpg(proc.pid, s)
				s = signal.SIGTERM
				ss = "SIGTERM"
				proc.terminate()
			except:
				pass
			print >> base.Env.log, "Waiting for service '%s'" % (self.name)
			if base.Env.wait: proc.wait()
			while not procwait(proc):
				try:
					print >> base.Env.log, "Terminating service (%s) '%s'" % (ss, self.name)
					os.killpg(proc.pid, s)
					s = signal.SIGKILL
					ss = "SIGKILL"
					proc.terminate()
				except:
					pass

	def portfree(self, port):
		s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		try:
			s.connect(("127.0.0.2", port))
		except:
			pass
		else:
			raise ServiceException("Cannot start service '%s', port 127.0.0.2:%i already in use" % (self.name, port))
		finally:
			s.close()

	def waitconnect(self, port):
		timeout = 5*10
		while True:
			if None != self.proc.poll():
				raise ServiceException("Service %s died before we could establish a connection" % (self.name))
			s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
			try:
				s.connect(("127.0.0.2", port))
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
		self.log = self.tests.PrepareFile("log/service-%s.log" % self.name, "")
		self.failed = True
		self.Prepare()
		self.failed = False

	def _cleanup(self):
		self.kill()
		if not base.Env.force_cleanup and self.failed:
			return
		self.tests.CleanupFile("log/service-%s.log" % self.name)
		self.tests.CleanupFile("log/strace-%s.log" % self.name)
		self.tests.CleanupFile("log/truss-%s.log" % self.name)
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

class Lighttpd(Service):
	name = "lighttpd"

	def TestConfig(self):
		logfile = open(self.log, "w")
		inp = self.devnull()
		args = [base.Env.worker, '-m', base.Env.plugindir, '-c', base.Env.lighttpdconf, '-t']
		print >> base.Env.log, "Testing lighttpd config: %s" % (' '.join(args))
		proc = subprocess.Popen(args, stdin = inp, stdout = logfile, stderr = logfile, close_fds = True)
		if None != inp: inp.close()
		logfile.close()
		status = proc.wait()
		if 0 != status:
			os.system("cat '%s'" % self.log)
			raise BaseException("testing lighttpd config failed with returncode %i" % (status))

	def Prepare(self):
		self.TestConfig()

		self.portfree(base.Env.port)
		if base.Env.no_angel:
			if base.Env.valgrind:
				self.fork('valgrind', base.Env.worker, '-m', base.Env.plugindir, '-c', base.Env.lighttpdconf)
			else:
				self.fork(base.Env.worker, '-m', base.Env.plugindir, '-c', base.Env.lighttpdconf)
		else:
			self.fork(base.Env.angel, '-m', base.Env.plugindir, '-c', base.Env.angelconf)
		self.waitconnect(base.Env.port)

class FastCGI(Service):
	binary = [None]

	def __init__(self):
		self.sockfile = os.path.join(base.Env.dir, "tmp", "sockets", self.name + ".sock")
		super(FastCGI, self).__init__()

	def Prepare(self):
		sockdir = self.tests.PrepareDir(os.path.join("tmp", "sockets"))
		sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
		sock.bind(os.path.relpath(self.sockfile))
		sock.listen(8)
		self.fork(*self.binary, inp = sock)

	def Cleanup(self):
		if None != self.sockfile:
			try:
				os.remove(self.sockfile)
			except BaseException, e:
				print >>sys.stderr, "Couldn't delete socket '%s': %s" % (self.sockfile, e)
		self.tests.CleanupDir(os.path.join("tmp", "sockets"))
