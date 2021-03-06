/* vim: ts=4 sw=4 sts=4 sta tw=80 list
 *
 * The start up of the server, error out handlicng and signals
 * See Copyright Notice at the end of the file
 *
 */


#include <pthread.h>        /* mutexes, conditions  */
#include <sys/socket.h>     /* cn_strct->net_socket */

#include "lua.h"            /* lua_State */

/* A few constants */
#define BACK_LOG             8    /* How many sockets waiting for accept */

#define RECV_BUFF_LENGTH     8196 /* Main buffer assigned to each connection */
#define INIT_CONNS           3    /* start with 2^4 preallocated connections*/
#define ANSWER_LENGTH        256  /* length of ipc buffer */
#define WORKER_THREADS       2    /* keep that close to #CPU cores */
#define HTTP_VERSION         "HTTP/1.1"  /* we answer in HTTP 1.1 */
#define DEBUG_VERBOSE        2    /* dirty printfs all over the place */

/* These are preference that can be over ridden by args or configs */
#define APP_PORT             8000 /* for now a simple static value */
#define APP_ROOT             "sample" /* the standard directory fr apps */

#define STATIC_ROOT          "/static"  /* if your URL starts with that, it's static */
#define STATIC_ROOT_LENGTH   7         /* used for string matches */


/* ########################### DATA STRUCTURES ############################# */
enum bool {
	false,
	true
};

enum req_states {
	REQSTATE_READ_HEAD,  /* ready to read from the socket */
	REQSTATE_SEND_HEAD,  /* ready to send HTTP header for static content */
	REQSTATE_BUFF_HEAD,  /* ready to enqueue connection for dyn processing */
	REQSTATE_BUFF_FILE,  /* ready to read chunk of static file into buffer */
	REQSTATE_SEND_FILE   /* ready to send buffer back to client */
};

enum req_types {
	REQTYPE_GET,
	REQTYPE_HEAD,
	REQTYPE_POST,
	REQTYPE_PUT,
	REQTYPE_OPTIONS,
	REQTYPE_DELETE
};

enum http_version {
	HTTP_09,
	HTTP_10,
	HTTP_11
};

/* what we need to pass on to the worker thread(s) */
struct thread_arg {
	int        r_pipe;   /* let main loop to listen to thread messages */
	int        w_pipe;   /* let thread send messages to main loop */
	int        t_id;     /* the thread id */
	pthread_t  thread;   /* the actual thread */
};


/* contains all metadata regarding one connection */
struct cn_strct {
	/* doubly linked list for the _Busy_conns */
	struct  cn_strct     *c_next;
	struct  cn_strct     *c_prev;
	/* single linked list for queue FIFO styled */
	struct  cn_strct     *q_prev;
	/* basic information */
	enum    req_states    req_state;
	int                   net_socket;
	int                   file_desc;
	/* data buffer */
	char                 *data_buf_head;    /* points to start, always */
	char                 *data_buf;         /* points to current spot */
	const char           *out_buf;          /* points to a Lua buffer */
	size_t                processed_bytes;  /* read or write, how much is done? */
	/* inc buffer state */
	int                   line_count;
	/* head information */
	enum    req_types     req_type;
	char                 *url;              /* points to pos in buffer, where url starts */
	char                 *get_str;          /* what's past the ? in url */
	char                 *post_str;         /* what's past the body */
	enum    http_version  http_prot;        /* not that we would care ...*/

	enum    bool          is_static;        /* serve a file, don't run app */
	int                   id;               /* identify which came back from thread */
};


/* ######################## FUNCTION DECLARATIONS ########################## */
/* debug - visualize cn and linked lists */
#if DEBUG_VERBOSE == 2
void  print_list             ( struct cn_strct *nd );
void  print_queue            ( struct cn_strct *nd, int count );
void  print_cn               ( struct cn_strct *cn );
#endif

/* string parsing methods -> disect HTTP header */
void  parse_first_line       ( struct cn_strct *cn );
const char *getmimetype      ( const char *name );

/* application bound stuff */
void *run_app_thread         ( void *tid );

/* The main loop in server called from main */
void server_loop();

/* ######################## GLOBAL VARIABLES ############################### */
extern struct cn_strct     *_Free_conns;       /* idleing conns, LIFO stack */
extern int                  _Free_count;
extern struct cn_strct     *_Busy_conns;       /* working conns, doubly linked list */
extern int                  _Busy_count;
extern struct cn_strct*    *_All_conns;        /* array with all conns */
extern const char * const   _Server_version;
extern int                  _Master_sock;      /* listening master socket */
extern time_t               _Last_loop;        /* marks the last run of select */
extern char                 _Master_date[30];  /* the formatted date */
extern int                  _Conn_count;       /* all existing cn_structs */
extern int                  _Conn_size;        /* 2^x connections */

/* a FIFO stack for quead up conns waiting for threads */
extern struct cn_strct     *_Queue_head;
extern struct cn_strct     *_Queue_tail;
extern int                  _Queue_count;
extern pthread_mutex_t      wake_worker_mutex;
extern pthread_mutex_t      pull_job_mutex;
extern pthread_cond_t       wake_worker_cond;
//extern pthread_t            _Workers[WORKER_THREADS]; /* used to clean up */
extern struct thread_arg    _Workers[WORKER_THREADS]; /* used to clean up */
//extern struct thread_arg    _Thread_args[WORKER_THREADS]; /* holds the pipes */


/*
 * Copyright (C) 2009, Tobias Kieslich. All rights reseved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining
 *  a copy of this software and associated documentation files (the
 *  "Software"), to deal in the Software without restriction, including
 *  without limitation the rights to use, copy, modify, merge, publish,
 *  distribute, sublicense, and/or sell copies of the Software, and to
 *  permit persons to whom the Software is furnished to do so, subject to
 *  the following conditions:
 *
 *  The above copyright notice and this permission notice shall be
 *  included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
