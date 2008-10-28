#ifndef _LIGHTTPD_TYPEDEFS_H_
#define _LIGHTTPD_TYPEDEFS_H_

typedef enum {
	HTTP_TRANSFER_ENCODING_IDENTITY,
	HTTP_TRANSFER_ENCODING_CHUNKED
} transfer_encoding_t;

typedef enum {
	HANDLER_GO_ON,
	HANDLER_FINISHED,
	HANDLER_COMEBACK,
	HANDLER_WAIT_FOR_EVENT,
	HANDLER_ERROR,
	HANDLER_WAIT_FOR_FD
} handler_t;

/* structs from headers, in alphabetic order */

/* actions.h */

struct action;
typedef struct action action;

struct action_stack;
typedef struct action_stack action_stack;

/* chunk.h */

struct chunkfile;
typedef struct chunkfile chunkfile;

struct chunk;
typedef struct chunk chunk;

struct chunkqueue;
typedef struct chunkqueue chunkqueue;

struct chunkiter;
typedef struct chunkiter chunkiter;

/* chunk_parser.h */

struct chunk_parser_ctx;
typedef struct chunk_parser_ctx chunk_parser_ctx;

struct chunk_parser_mark;
typedef struct chunk_parser_mark chunk_parser_mark;

/* condition.h */

struct condition_rvalue;
typedef struct condition_rvalue condition_rvalue;

struct condition_lvalue;
typedef struct condition_lvalue condition_lvalue;

struct condition;
typedef struct condition condition;

/* connection.h */

struct connection;
typedef struct connection connection;

/* hhtp_headers.h */

struct http_header;
typedef struct http_header http_header;

struct http_headers;
typedef struct http_headers http_headers;

/* options.h */

struct option_set;
typedef struct option_set option_set;

union option_value;
typedef union option_value option_value;

/* plugin.h */
struct plugin;
typedef struct plugin plugin;

struct plugin_option;
typedef struct plugin_option plugin_option;

struct server_option;
typedef struct server_option server_option;

struct plugin_action;
typedef struct plugin_action plugin_action;

struct server_action;
typedef struct server_action server_action;

struct plugin_setup;
typedef struct plugin_setup plugin_setup;

struct server_setup;
typedef struct server_setup server_setup;

/* request.h */

typedef enum {
	HTTP_METHOD_UNSET = -1,
	HTTP_METHOD_GET,
	HTTP_METHOD_POST,
	HTTP_METHOD_HEAD,
	HTTP_METHOD_OPTIONS,
	HTTP_METHOD_PROPFIND,  /* WebDAV */
	HTTP_METHOD_MKCOL,
	HTTP_METHOD_PUT,
	HTTP_METHOD_DELETE,
	HTTP_METHOD_COPY,
	HTTP_METHOD_MOVE,
	HTTP_METHOD_PROPPATCH,
	HTTP_METHOD_REPORT, /* DeltaV */
	HTTP_METHOD_CHECKOUT,
	HTTP_METHOD_CHECKIN,
	HTTP_METHOD_VERSION_CONTROL,
	HTTP_METHOD_UNCHECKOUT,
	HTTP_METHOD_MKACTIVITY,
	HTTP_METHOD_MERGE,
	HTTP_METHOD_LOCK,
	HTTP_METHOD_UNLOCK,
	HTTP_METHOD_LABEL,
	HTTP_METHOD_CONNECT
} http_method_t;

typedef enum {
	HTTP_VERSION_UNSET = -1,
	HTTP_VERSION_1_0,
	HTTP_VERSION_1_1
} http_version_t;

struct request;
typedef struct request request;

struct request_uri;
typedef struct request_uri request_uri;

struct physical;
typedef struct physical physical;

/* respone.h */

struct response;
typedef struct response response;

/* server.h */

struct server;
typedef struct server server;

/* value.h */

struct value;
typedef struct value value;

typedef enum {
	VALUE_NONE,
	VALUE_BOOLEAN,
	VALUE_NUMBER,
	VALUE_STRING,
	VALUE_LIST,
	VALUE_HASH,
	VALUE_ACTION,     /**< shouldn't be used for options, but may be needed for constructing actions */
	VALUE_CONDITION   /**< shouldn't be used for options, but may be needed for constructing actions */
} value_type;

/* virtualrequest.h */

struct vrequest;
typedef struct vrequest vrequest;

struct filter;
typedef struct filter filter;

struct filters;
typedef struct filters filters;

/* worker.h */

struct worker;
typedef struct worker worker;

#endif
