
#ifndef LUARPC_H
#define LUARPC_H

#include <stdio.h>
#include "cexcept.h"
#include "type.h"
#include "serial.h"

#include "platform_conf.h"
/****************************************************************************/
// Parameters

#define NUM_FUNCNAME_CHARS 20 // Maximum function name length

#define MAX_LINK_ERRS ( 2 ) // Maximum number of framing errors before connection reset

#if defined( LUARPC_ENABLE_SERIAL )
#define LUARPC_MODE "serial"
#define tpt_handler ser_handler
#elif defined( LUARPC_ENABLE_SOCKET )
#define LUARPC_MODE "tcpip"
typedef int tpt_handler;
#define MAXCON ( 1 )
#else
#error "No RPC mode Selected.."
#endif

// a kind of silly way to get the maximum int, but oh well ...
#define MAXINT ((int)((((unsigned int)(-1)) << 1) >> 1))

/****************************************************************************/
// Debug Error Handling

// allow special handling for GCC compiler
#ifdef __GNUC__
#define DOGCC(x) x
#else
#define DOGCC(x) /* */
#endif

// Assertions for debug mode
#ifdef LUARPC_DEBUG
#ifdef __GNUC__
#define MYASSERT(a) if (!(a)) rpcdebug ( \
  "assertion \"" #a "\" failed in %s() [%s]",__FUNCTION__,__FILE__);
#else
#define MYASSERT(a) if (!(a)) rpcdebug ( \
  "assertion \"" #a "\" failed in %s:%d",__FILE__,__LINE__);
#endif
#else
#define MYASSERT(a) ;
#endif

//****************************************************************************
// Error Messages & Exceptions

#ifdef WIN32_BUILD
#include "windows.h"
#define transport_strerror strerror
#define transport_errno (GetLastError())
#else
#define transport_errno errno
#define transport_strerror strerror
#endif

// error numbers passed around are normal system "errno" error numbers
//  (normally generated by transport operations), except when they have the
//  following values:

enum {
  ERR_EOF       = MAXINT - 100,  // reached end of file on transport
  ERR_CLOSED    = MAXINT - 101,  // attempted operation on closed transport
  ERR_PROTOCOL  = MAXINT - 102,  // some error in the received protocol
  ERR_NODATA    = MAXINT - 103,
  ERR_COMMAND   = MAXINT - 106,
  ERR_HEADER    = MAXINT - 107,
  ERR_LONGFNAME = MAXINT - 108
};

enum exception_type { done, nonfatal, fatal };

struct exception {
  enum exception_type type;
	int errnum;
};

define_exception_type(struct exception);

extern struct exception_context the_exception_context[ 1 ];

//****************************************************************************
// LuaRPC Structures

// Transport Connection Structure
typedef struct _Transport Transport;
struct _Transport 
{
  tpt_handler fd;
  unsigned tmr_id;
  u32    loc_little: 1,               // Local is little endian?
         loc_armflt: 1,               // local float representation is arm float?
         loc_intnum: 1,               // Local is integer only?
         net_little: 1,               // Network is little endian?
         net_intnum: 1;               // Network is integer only?
  u8     lnum_bytes;
  FILE* file;
  int is_set;
  int must_die;
};

typedef struct _Handle Handle;
struct _Handle 
{
  Transport tpt;                      // the handle socket
  int error_handler;                  // function reference
  int async;                          // nonzero if async mode being used
  int read_reply_count;               // number of async call return values to read
};

typedef struct _Helper Helper;
struct _Helper {
  Handle *handle;                     // pointer to handle object
	Helper *parent;                     // parent helper
  int pref;                           // Parent reference idx in registry
	u8 nparents;                        // number of parents
  char funcname[NUM_FUNCNAME_CHARS];  // name of the function
};

typedef struct _ServerHandle ServerHandle;
struct _ServerHandle {
  Transport ltpt;   // listening transport, always valid if no error
  //  Transport atpt;   // accepting transport, valid if connection established
	int link_errs;
};


// Connection State Checking
#ifdef WIN32_BUILD
#define INVALID_TRANSPORT (INVALID_HANDLE_VALUE)
#else
#define INVALID_TRANSPORT (-1)
#endif

#define TRANSPORT_VERIFY_OPEN \
	if (tpt->fd == INVALID_TRANSPORT) \
	{ \
		e.errnum = ERR_CLOSED; \
		e.type = fatal; \
		Throw( e ); \
	}

// Arg & Error Checking Provided to Transport Mechanisms 
int check_num_args (lua_State *L, int desired_n);
void deal_with_error (lua_State *L, const char *error_string);
//void my_lua_error( lua_State *L, const char *errmsg );

// TRANSPORT API 

// Setup Transport 
void transport_init (Transport *tpt);

// Open Listener / Server 
void transport_open_listener(lua_State *L, ServerHandle *handle);

// Open Connection / Client 
int transport_open_connection(lua_State *L, Handle *handle);

// Accept Connection 
void transport_accept (Transport *tpt, Transport *atpt);

// Read & Write to Transport 
void transport_read_buffer (Transport *tpt, u8 *buffer, int length);
void transport_write_buffer (Transport *tpt, const u8 *buffer, int length);

// Check if data is available on connection without reading:
// 		- 1 = data available, 0 = no data available
int transport_readable (Transport *tpt);

// Check if transport is open:
//		- 1 = connection open, 0 = connection closed
int transport_is_open (Transport *tpt);

// Shut down connection
void transport_delete (Transport *tpt);
Transport * transport_create (void);


void transport_flush(Transport *tpt);

struct transport_node {
  Transport* t;
  struct transport_node *prev;
  struct transport_node *next;
};
int transport_select(struct transport_node* transports);



struct transport_node* transport_new_list();
void transport_insert_to_list(struct transport_node* head, Transport* t);
struct transport_node* transport_remove_from_list(struct transport_node* head, Transport* t);
extern struct transport_node* transport_list;
#endif

