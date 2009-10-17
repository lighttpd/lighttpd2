#ifndef _LIGHTTPD_ANGEL_TYPEDEFS_H_
#define _LIGHTTPD_ANGEL_TYPEDEFS_H_

/* angel_proc.h */

typedef struct liErrorPipe liErrorPipe;
typedef struct liProc liProc;

/* angel_server.h */

typedef enum {
	LI_INSTANCE_DOWN,       /* not started yet */
	LI_INSTANCE_SUSPENDED,  /* inactive, neither accept nor logs, handle remaining connections */
	LI_INSTANCE_WARMUP,     /* only accept(), no logging: waiting for another instance to suspend */
	LI_INSTANCE_RUNNING,    /* everything running */
	LI_INSTANCE_SUSPENDING, /* suspended accept(), still logging, handle remaining connections  */
	LI_INSTANCE_FINISHED    /* not running */
} liInstanceState;

typedef struct liServer liServer;
typedef struct liInstance liInstance;
typedef struct liInstanceConf liInstanceConf;
typedef struct liInstanceResource liInstanceResource;

#endif
