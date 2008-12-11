## our modules are without the "lib" prefix

MACRO(ADD_AND_INSTALL_LIBRARY LIBNAME SRCFILES)
	IF(BUILD_STATIC)
		ADD_LIBRARY(${LIBNAME} STATIC ${SRCFILES})
		TARGET_LINK_LIBRARIES(lighttpd ${LIBNAME})
	ELSE(BUILD_STATIC)
		ADD_LIBRARY(${LIBNAME} SHARED ${SRCFILES})
		SET(L_INSTALL_TARGETS ${L_INSTALL_TARGETS} ${LIBNAME})

		ADD_TARGET_PROPERTIES(${LIBNAME} LINK_FLAGS ${COMMON_LDFLAGS})
		ADD_TARGET_PROPERTIES(${LIBNAME} COMPILE_FLAGS ${COMMON_CFLAGS})
		SET_TARGET_PROPERTIES(${LIBNAME} PROPERTIES CMAKE_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

		## Windows likes to link it this way back to app!
		IF(WIN32)
			SET_TARGET_PROPERTIES(${LIBNAME} PROPERTIES LINK_FLAGS lighttpd.lib)
		ENDIF(WIN32)

		IF(APPLE)
			SET_TARGET_PROPERTIES(${LIBNAME} PROPERTIES LINK_FLAGS "-flat_namespace -undefined suppress")
		ENDIF(APPLE)
	ENDIF(BUILD_STATIC)
ENDMACRO(ADD_AND_INSTALL_LIBRARY)

MACRO(ADD_TARGET_PROPERTIES _target _name)
	SET(_properties)
	FOREACH(_prop ${ARGN})
		SET(_properties "${_properties} ${_prop}")
	ENDFOREACH(_prop)
	GET_TARGET_PROPERTY(_old_properties ${_target} ${_name})
	MESSAGE("adding property to ${_target} ${_name}:" ${_properties})
	IF(NOT _old_properties)
		# in case it's NOTFOUND
		SET(_old_properties)
	ENDIF(NOT _old_properties)
	SET_TARGET_PROPERTIES(${_target} PROPERTIES ${_name} "${_old_properties} ${_properties}")
ENDMACRO(ADD_TARGET_PROPERTIES)
