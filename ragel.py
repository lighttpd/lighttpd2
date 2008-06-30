#! /usr/bin/env python
# encoding: utf-8
# Thomas Nagy, 2006 (ita)

"Ragel: '.rl' files are converted into .c files using 'ragel': {.rl -> .c -> .o}"

import TaskGen

TaskGen.declare_chain(
	name = 'ragel',
	action = '${RAGEL} -o ${TGT} ${SRC}',
	ext_in = '.rl',
	ext_out = '.c',
	before = 'c',
)

def detect(conf):
	dang = conf.find_program('ragel', var='RAGEL')
	if not dang: conf.fatal('cannot find the program "ragel"')
