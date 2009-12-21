#ifndef _LIGHTTPD_OPTIONS_LUA_H_
#define _LIGHTTPD_OPTIONS_LUA_H_

#include <lighttpd/base.h>
#include <lua.h>

/* converts the top of the stack into an value
 * and pops the value
 * returns NULL if it couldn't convert the value (still pops it)
 */
LI_API liValue* li_value_from_lua(liServer *srv, lua_State *L);

/* always returns 1, pushes nil on error */
LI_API int li_lua_push_value(lua_State *L, liValue *value);

LI_API GString* li_lua_togstring(lua_State *L, int ndx);

#endif
