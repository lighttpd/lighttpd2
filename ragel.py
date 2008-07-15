#! /usr/bin/env python
# encoding: utf-8

"Ragel: '.rl' files are converted into .c files using 'ragel': {.rl -> .c -> .o}"

import TaskGen, Task, Runner


def rageltaskfun(task):
	env = task.env
	ragelbin = env.get_flat('RAGEL')
	if ragelbin:
		cmd = '%s -o %s %s' % (ragelbin, task.outputs[0].bldpath(env), task.inputs[0].srcpath(env))
	else:
		src = task.inputs[0].srcpath(env)
		src = src[:src.rfind('.')] + '.c'
		cmd = 'cp %s %s' % (src, task.outputs[0].bldpath(env))
	return Runner.exec_command(cmd)

rageltask = Task.task_type_from_func('ragel', rageltaskfun, vars = ['RAGEL'], color = 'BLUE', ext_in = '.rl', ext_out = '.c', before = 'c')

@TaskGen.extension('.rl')
@TaskGen.before('c')
def ragel(self, node):
	out = node.change_ext('.c')
	self.allnodes.append(out)
	tsk = self.create_task('ragel')
	tsk.set_inputs(node)
	tsk.set_outputs(out)

def detect(conf):
	dang = conf.find_program('ragel', var='RAGEL')
