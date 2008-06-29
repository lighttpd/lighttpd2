void action_stack_init(action_stack* as)
{
	// preallocate a stack of 15 elements
	as->stack = g_array_sized_new(FALSE, TRUE, sizeof(action_stack_elem), 15);
	as->index = 0;
}

void action_stack_push(action_stack* as, action_stack_elem ase)
{
	// stack needs to grow
	if (as->index == (as->stack->len -1)) // we are at the end of the stack
		g_array_append_val(as, ase);
	else
		g_array_insert_val(as, as->index, ase);

	as->index++;
}

// pops the last entry of the action stack
action_stack_elem action_stack_pop(action_stack* as)
{
	as->index--;
	return g_array_index(as, action_stack_elem, as->index);
}

action_result action_list_exec(action_list* al, action_stack* as, guint index)
{
	action_stack_elem ase;
	guint i;
	action* act;
	action_result ar;

	// iterate over list
	for (i = index; i < al->list->len; i++)
	{
		act = g_array_index(al->list, action*, i);

		switch (act->type)
		{
			case ACTION_CONDITION:
				if (TRUE == condition_check(&(act->value.cond)))
				{
					// save current
					ase.al = al;
					ase.index = i;
					action_stack_push(as, ase);

					ar = action_list_exec(act->target, as);
					break;
				}
				else
					continue;
			case ACTION_SETTING:
				ar = ACTION_RESULT_GO_ON;
				break;
			case ACTION_FUNCTION:
				ar = ACTION_RESULT_GO_ON;
				break;
			default:
				ar = ACTION_RESULT_GO_ON;
				// TODO: print error and exit
		}

		if (ar == ACTION_RESULT_BREAK)
			break;
		else if (ar == ACTION_RESULT_WAIT_FOR_EVENT)
			return ACTION_RESULT_WAIT_FOR_EVENT;
	}

	// executed all actions in the list
	// if the action stack index is > 0, we need to jump back to the previous list
	if (as->index > 0)
	{
		ase = action_stack_pop(as);
		return action_list_exec(ase->al, ase->index);
	}

	return ACTION_RESULT_GO_ON;
}

// checks if a condition is fulfilled. returns the next action to jump to if fulfilled, otherwise NULL
gboolean condition_check(condition* cond)
{
	switch (cond->type)
	{
		case CONDITION_STRING:
			return condition_check_string(cond);
		case CONDITION_INT:
			return condition_check_int(cond);
		case CONDITION_BOOL:
			return condition_check_bool(cond);
		case CONDITION_IP:
			// todo
		default:
			// TODO: print error and exit
			return FALSE;
	}
}


// string condition, operators: ==, !=, =~, !~
gboolean condition_check_string(condition* cond)
{
	switch (cond->op)
	{
		case CONDITION_EQUAL:
			if (cond->lvalue.val_string->len != cond->rvalue.val_string->len)
				return FALSE;
			return g_string_equal(cond->lvalue.val_string, cond->rvalue.val_string);
		case CONDITION_UNEQUAL:
			return (FALSE == g_string_equal(cond->lvalue.val_string, cond->rvalue.val_string)) ? TRUE : FALSE;
		case CONDITION_REGEX_MATCH:
			// todo
		case CONDITION_REGEX_NOMATCH:
			// todo
		default:
			// TODO: print error and exit
			return FALSE;
	}
}

// integer condition, operators: ==, !=, <, <=, >, >=
gboolean condition_check_int(condition* cond)
{
	switch (cond->op)
	{
		case CONDITION_EQUAL:
			return (cond->lvalue.val_int == cond->rvalue.val_int) ? TRUE : FALSE;
		case CONDITION_UNEQUAL:
			return (cond->lvalue.val_int != cond->rvalue.val_int) ? TRUE : FALSE;
		case CONDITION_LESS:
			return (cond->lvalue.val_int < cond->rvalue.val_int) ? TRUE : FALSE;
		case CONDITION_LESS_EQUAL:
			return (cond->lvalue.val_int <= cond->rvalue.val_int) ? TRUE : FALSE;
		case CONDITION_GREATER:
			return (cond->lvalue.val_int > cond->rvalue.val_int) ? TRUE : FALSE;
		case CONDITION_GREATER_EQUAL:
			return (cond->lvalue.val_int >= cond->rvalue.val_int) ? TRUE : FALSE;
		default:
			// TODO: print error and exit
			return FALSE;
	}
}

// bool condition, operators: ==, !=
gboolean condition_check_bool(condition* cond)
{
	switch (cond->op)
	{
		case CONDITION_EQUAL:
			return (cond->lvalue.val_bool == cond->rvalue.val_bool) ? TRUE : FALSE;
		case CONDITION_UNEQUAL:
			return (cond->lvalue.val_bool != cond->rvalue.val_bool) ? TRUE : FALSE;
		default:
			// TODO: print error and exit
			return FALSE;
	}
}
