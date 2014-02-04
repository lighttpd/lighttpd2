#ifndef _LIGHTTPD_TYPEDEFS_H_
#define _LIGHTTPD_TYPEDEFS_H_

typedef enum {
	LI_HTTP_TRANSFER_ENCODING_IDENTITY,
	LI_HTTP_TRANSFER_ENCODING_CHUNKED
} liTransferEncoding;

typedef enum {
	LI_HANDLER_GO_ON,
	LI_HANDLER_COMEBACK,
	LI_HANDLER_WAIT_FOR_EVENT,
	LI_HANDLER_ERROR
} liHandlerResult;

typedef enum { LI_TRIFALSE, LI_TRIMAYBE, LI_TRITRUE } liTristate;

typedef enum { LI_GMTIME, LI_LOCALTIME } liTimeFunc;
typedef enum { LI_TS_FORMAT_DEFAULT, LI_TS_FORMAT_HEADER } liTSFormat;

/* structs from headers, in alphabetic order */

/* actions.h */

typedef struct liAction liAction;

typedef struct liActionStack liActionStack;

typedef struct liActionRegexStackElement liActionRegexStackElement;

typedef struct liActionFunc liActionFunc;

typedef struct liBalancerFunc liBalancerFunc;

typedef enum {
	LI_ACTION_TNOTHING,
	LI_ACTION_TSETTING,
	LI_ACTION_TSETTINGPTR,
	LI_ACTION_TFUNCTION,
	LI_ACTION_TCONDITION,
	LI_ACTION_TLIST,
	LI_ACTION_TBALANCER
} liActionType;

typedef enum {
	LI_BACKEND_OVERLOAD,
	LI_BACKEND_DEAD
} liBackendError;

/* base_lua.h */

typedef struct liLuaState liLuaState;

/* chunk.h */

typedef struct liChunkFile liChunkFile;

typedef struct liChunk liChunk;

typedef struct liCQLimit liCQLimit;

typedef struct liChunkQueue liChunkQueue;

typedef struct liChunkIter liChunkIter;

/* chunk_parser.h */

typedef struct liChunkParserCtx liChunkParserCtx;

typedef struct liChunkParserMark liChunkParserMark;

/* condition.h */

typedef struct liConditionRValue liConditionRValue;

typedef struct liConditionLValue liConditionLValue;

typedef struct liCondition liCondition;

/* connection.h */

typedef struct liConnection liConnection;

/* filter.h */

typedef struct liFilter liFilter;

/* http_headers.h */

typedef struct liHttpHeader liHttpHeader;

typedef struct liHttpHeaders liHttpHeaders;

/* li_http_request_parser.h */

typedef struct liHttpRequestCtx liHttpRequestCtx;

/* li_http_response_parser.h */

typedef struct liHttpResponseCtx liHttpResponseCtx;

/* log.h */

typedef struct liLogTarget liLogTarget;
typedef struct liLogEntry liLogEntry;
typedef struct liLogServerData liLogServerData;
typedef struct liLogWorkerData liLogWorkerData;
typedef struct liLogMap liLogMap;
typedef struct liLogContext liLogContext;

typedef enum {
	LI_LOG_LEVEL_DEBUG = 0,
	LI_LOG_LEVEL_INFO,
	LI_LOG_LEVEL_WARNING,
	LI_LOG_LEVEL_ERROR,
	LI_LOG_LEVEL_ABORT,
	LI_LOG_LEVEL_BACKEND
} liLogLevel;

#define LI_LOG_LEVEL_COUNT (1 + (unsigned int) LI_LOG_LEVEL_BACKEND)

typedef enum {
	LI_LOG_TYPE_STDERR,
	LI_LOG_TYPE_FILE,
	LI_LOG_TYPE_PIPE,
	LI_LOG_TYPE_SYSLOG,
	LI_LOG_TYPE_NONE
} liLogType;

/* network.h */

typedef enum {
	LI_NETWORK_STATUS_SUCCESS,             /**< socket probably could have done more */
	LI_NETWORK_STATUS_FATAL_ERROR,
	LI_NETWORK_STATUS_CONNECTION_CLOSE,
	LI_NETWORK_STATUS_WAIT_FOR_EVENT       /**< read/write returned -1 with errno=EAGAIN/EWOULDBLOCK */
} liNetworkStatus;

/* options.h */

typedef union liOptionValue liOptionValue;

typedef struct liOptionPtrValue liOptionPtrValue;

typedef struct liOptionSet liOptionSet;

typedef struct liOptionPtrSet liOptionPtrSet;

/* plugin.h */
typedef struct liPlugin liPlugin;

typedef struct liPluginOption liPluginOption;

typedef struct liServerOption liServerOption;

typedef struct liPluginOptionPtr liPluginOptionPtr;

typedef struct liServerOptionPtr liServerOptionPtr;

typedef struct liPluginAction liPluginAction;

typedef struct liServerAction liServerAction;

typedef struct liPluginSetup liPluginSetup;

typedef struct liServerSetup liServerSetup;

typedef struct liPluginAngel liPluginAngel;

/* request.h */

typedef enum {
	LI_HTTP_METHOD_UNSET = -1,
	LI_HTTP_METHOD_GET,
	LI_HTTP_METHOD_POST,
	LI_HTTP_METHOD_HEAD,
	LI_HTTP_METHOD_OPTIONS,
	LI_HTTP_METHOD_PROPFIND,  /* WebDAV */
	LI_HTTP_METHOD_MKCOL,
	LI_HTTP_METHOD_PUT,
	LI_HTTP_METHOD_DELETE,
	LI_HTTP_METHOD_COPY,
	LI_HTTP_METHOD_MOVE,
	LI_HTTP_METHOD_PROPPATCH,
	LI_HTTP_METHOD_REPORT, /* DeltaV */
	LI_HTTP_METHOD_CHECKOUT,
	LI_HTTP_METHOD_CHECKIN,
	LI_HTTP_METHOD_VERSION_CONTROL,
	LI_HTTP_METHOD_UNCHECKOUT,
	LI_HTTP_METHOD_MKACTIVITY,
	LI_HTTP_METHOD_MERGE,
	LI_HTTP_METHOD_LOCK,
	LI_HTTP_METHOD_UNLOCK,
	LI_HTTP_METHOD_LABEL,
	LI_HTTP_METHOD_CONNECT
} liHttpMethod;

typedef enum {
	LI_HTTP_VERSION_UNSET = -1,
	LI_HTTP_VERSION_1_0,
	LI_HTTP_VERSION_1_1
} liHttpVersion;

typedef struct liRequest liRequest;

typedef struct liRequestUri liRequestUri;

typedef struct liPhysical liPhysical;

/* respone.h */

typedef struct liResponse liResponse;

/* server.h */

typedef struct liServerStateWait liServerStateWait;

typedef struct liServer liServer;

typedef struct liServerSocket liServerSocket;

/* stream.h */

typedef struct liStream liStream;
typedef struct liIOStream liIOStream;

typedef enum {
	LI_STREAM_NEW_DATA, /* either new/more data in stream->source->cq, or more data to be generated */
	LI_STREAM_NEW_CQLIMIT,
	LI_STREAM_CONNECTED_DEST,
	LI_STREAM_CONNECTED_SOURCE,
	LI_STREAM_DISCONNECTED_DEST,
	LI_STREAM_DISCONNECTED_SOURCE,
	LI_STREAM_DESTROY
} liStreamEvent;

typedef enum {
	LI_IOSTREAM_READ, /* should try reading */
	LI_IOSTREAM_WRITE, /* should try writing */
	LI_IOSTREAM_CONNECTED_DEST, /* stream_in connected dest */
	LI_IOSTREAM_CONNECTED_SOURCE, /* stream_out connected source */
	LI_IOSTREAM_DISCONNECTED_DEST, /* stream_in disconnected dest */
	LI_IOSTREAM_DISCONNECTED_SOURCE, /* stream_out disconnected source */
	LI_IOSTREAM_DESTROY /* stream_in and stream_out are down to refcount = 0 */
} liIOStreamEvent;

/* throttle.h */

typedef struct liThrottleState liThrottleState;
typedef struct liThrottlePool liThrottlePool;

/* virtualrequest.h */

typedef struct liConCallbacks liConCallbacks;

typedef struct liConInfo liConInfo;

typedef struct liVRequest liVRequest;

/* worker.h */

typedef struct liWorker liWorker;

typedef struct liStatCacheEntryData liStatCacheEntryData;
typedef struct liStatCacheEntry liStatCacheEntry;
typedef struct liStatCache liStatCache;

#endif
