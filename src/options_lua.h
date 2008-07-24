#ifndef _LIGHTTPD_OPTIONS_LUA_H_
#define _LIGHTTPD_OPTIONS_LUA_H_

#include "base.h"
#include "options.h"

#include <lua.h>

/* converts the top of the stack into an option
 * and pops the value
 * returns NULL if it couldn't convert the value (still pops it)
 */
LI_API option* option_from_lua(server *srv, lua_State *L);

LI_API GString* lua_togstring(lua_State *L, int ndx);

#endif
