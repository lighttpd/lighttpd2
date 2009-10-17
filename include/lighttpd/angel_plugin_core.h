#ifndef _LIGHTTPD_ANGEL_PLUGIN_CORE_H_
#define _LIGHTTPD_ANGEL_PLUGIN_CORE_H_

#include <lighttpd/angel_base.h>

typedef struct liPluginCoreConfig liPluginCoreConfig;
struct liPluginCoreConfig {
	/* Load */
	gboolean load_failed;
	liInstanceConf *load_instconf;
	GPtrArray *load_listen_masks;

	/* Running */
	liInstanceConf *instconf;
	GPtrArray *listen_masks;

	liInstance *inst;
	GHashTable *listen_sockets;

	ev_signal sig_hup;
};

typedef struct liPluginCoreListenMask liPluginCoreListenMask;
struct liPluginCoreListenMask {
	enum {
		LI_PLUGIN_CORE_LISTEN_MASK_IPV4,
		LI_PLUGIN_CORE_LISTEN_MASK_IPV6,
		LI_PLUGIN_CORE_LISTEN_MASK_UNIX
	} type;

	union {
		struct {
			guint32 addr;
			guint32 networkmask;
			guint16 port;
		} ipv4;
		struct {
			guint8 addr[16];
			guint network;
			guint16 port;
		} ipv6;
		struct {
			GString *path;
		} unix_socket;
	} value;
};

LI_API gboolean li_plugin_core_init(liServer *srv);

#endif
