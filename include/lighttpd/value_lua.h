#ifndef _LIGHTTPD_OPTIONS_LUA_H_
#define _LIGHTTPD_OPTIONS_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

/* converts the top of the stack into an value
 * and pops the value
 * returns NULL if it couldn't convert the value (still pops it)
 */
LI_API liValue* li_value_from_lua(liServer *srv, lua_State *L);

LI_API GString* li_lua_togstring(lua_State *L, int ndx);

#endif
