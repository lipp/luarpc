/*****************************************************************************
* Lua-RPC library, Copyright (C) 2001 Russell L. Smith. All rights reserved. *
*   Email: russ@q12.org   Web: www.q12.org                                   *
* For documentation, see http://www.q12.org/lua. For the license agreement,  *
* see the file LICENSE that comes with this distribution.                    *
*****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>

#ifdef WIN32

#include <windows.h>

#else /* not WIN32 */

#include <string.h>
#include <errno.h>
#include <alloca.h>
#include <signal.h>

/* for sockets */
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

#endif /* WIN32 */

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include "config.h"

/****************************************************************************/
/* parameters */

#define MAXCON 10	/* maximum number of waiting server connections */

/* a kind of silly way to get the maximum int, but oh well ... */
#define MAXINT ((int)((((unsigned int)(-1)) << 1) >> 1))

/****************************************************************************/
/* error handling */

/* allow special handling for GCC compiler */
#ifdef __GNUC__
#define DOGCC(x) x
#else
#define DOGCC(x) /* */
#endif


/* assertions */

#ifndef NDEBUG
#ifdef __GNUC__
#define MYASSERT(a) if (!(a)) debug ( \
  "assertion \"" #a "\" failed in %s() [%s]",__FUNCTION__,__FILE__);
#else
#define MYASSERT(a) if (!(a)) debug ( \
  "assertion \"" #a "\" failed in %s:%d",__FILE__,__LINE__);
#endif
#else
#define MYASSERT(a) ;
#endif


static void errorMessage (const char *msg, va_list ap)
{
  fflush (stdout);
  fflush (stderr);
  fprintf (stderr,"\nError: ");
  vfprintf (stderr,msg,ap);
  fprintf (stderr,"\n\n");
  fflush (stderr);
}


DOGCC(static void panic (const char *msg, ...)
      __attribute__ ((noreturn,unused));)
static void panic (const char *msg, ...)
{
  va_list ap;
  va_start (ap,msg);
  errorMessage (msg,ap);
  exit (1);
}


DOGCC(static void debug (const char *msg, ...)
      __attribute__ ((noreturn,unused));)
static void debug (const char *msg, ...)
{
  va_list ap;
  va_start (ap,msg);
  errorMessage (msg,ap);
  abort();
}

/****************************************************************************/
/* handle the differences between winsock and unix */

#ifdef WIN32

#define close closesocket
#define read(fd,buf,len) recv ((fd),(buf),(len),0)
#define write(fd,buf,len) send ((fd),(buf),(len),0)
#define SOCKTYPE SOCKET
#define sock_errno (WSAGetLastError())


/* check some assumptions */
#if SOCKET_ERROR >= 0
#error need SOCKET_ERROR < 0
#endif


/* this should be called before any network operations */

static void net_startup()
{
  WORD wVersionRequested;
  WSADATA wsaData;
  int err;

  // startup WinSock version 2
  wVersionRequested = MAKEWORD(2,0);
  err = WSAStartup (wVersionRequested,&wsaData);
  if (err != 0) panic ("could not start winsock");

  // confirm that the WinSock DLL supports 2.0. note that if the DLL
  // supports versions greater than 2.0 in addition to 2.0, it will
  // still return 2.0 in wVersion since that is the version we requested.
  if (LOBYTE (wsaData.wVersion ) != 2 ||
      HIBYTE(wsaData.wVersion) != 0 ) {
    WSACleanup();
    panic ("bad winsock version (< 2)");
  }
}


/* WinSock does not seem to have a strerror() style function, so here it is. */

const char * sock_strerror (int n)
{
  switch (n) {
  case WSAEACCES: return "Permission denied.";
  case WSAEADDRINUSE: return "Address already in use.";
  case WSAEADDRNOTAVAIL: return "Cannot assign requested address.";
  case WSAEAFNOSUPPORT:
    return "Address family not supported by protocol family.";
  case WSAEALREADY: return "Operation already in progress.";
  case WSAECONNABORTED: return "Software caused connection abort.";
  case WSAECONNREFUSED: return "Connection refused.";
  case WSAECONNRESET: return "Connection reset by peer.";
  case WSAEDESTADDRREQ: return "Destination address required.";
  case WSAEFAULT: return "Bad address.";
  case WSAEHOSTDOWN: return "Host is down.";
  case WSAEHOSTUNREACH: return "No route to host.";
  case WSAEINPROGRESS: return "Operation now in progress.";
  case WSAEINTR: return "Interrupted function call.";
  case WSAEINVAL: return "Invalid argument.";
  case WSAEISCONN: return "Socket is already connected.";
  case WSAEMFILE: return "Too many open files.";
  case WSAEMSGSIZE: return "Message too long.";
  case WSAENETDOWN: return "Network is down.";
  case WSAENETRESET: return "Network dropped connection on reset.";
  case WSAENETUNREACH: return "Network is unreachable.";
  case WSAENOBUFS: return "No buffer space available.";
  case WSAENOPROTOOPT: return "Bad protocol option.";
  case WSAENOTCONN: return "Socket is not connected.";
  case WSAENOTSOCK: return "Socket operation on nonsocket.";
  case WSAEOPNOTSUPP: return "Operation not supported.";
  case WSAEPFNOSUPPORT: return "Protocol family not supported.";
  case WSAEPROCLIM: return "Too many processes.";
  case WSAEPROTONOSUPPORT: return "Protocol not supported.";
  case WSAEPROTOTYPE: return "Protocol wrong type for socket.";
  case WSAESHUTDOWN: return "Cannot send after socket shutdown.";
  case WSAESOCKTNOSUPPORT: return "Socket type not supported.";
  case WSAETIMEDOUT: return "Connection timed out.";
  case WSAEWOULDBLOCK: return "Resource temporarily unavailable.";
  case WSAHOST_NOT_FOUND: return "Host not found.";
  case WSANOTINITIALISED: return "Successful WSAStartup not yet performed.";
  case WSANO_DATA: return "Valid name, no data record of requested type.";
  case WSANO_RECOVERY: return "This is a nonrecoverable error.";
  case WSASYSNOTREADY: return "Network subsystem is unavailable.";
  case WSATRY_AGAIN: return "Nonauthoritative host not found.";
  case WSAVERNOTSUPPORTED: return "Winsock.dll version out of range.";
  case WSAEDISCON: return "Graceful shutdown in progress.";
  default: return "Unknown error.";

  /* OS dependent error numbers? */
  /*
  case WSATYPE_NOT_FOUND: return "Class type not found.";
  case WSA_INVALID_HANDLE: return "Specified event object handle is invalid.";
  case WSA_INVALID_PARAMETER: return "One or more parameters are invalid.";
  case WSAINVALIDPROCTABLE:
    return "Invalid procedure table from service provider.";
  case WSAINVALIDPROVIDER: return "Invalid service provider version number.";
  case WSA_IO_INCOMPLETE:
    return "Overlapped I/O event object not in signaled state.";
  case WSA_IO_PENDING: return "Overlapped operations will complete later.";
  case WSA_NOT_ENOUGH_MEMORY: return "Insufficient memory available.";
  case WSAPROVIDERFAILEDINIT:
    return "Unable to initialize a service provider.";
  case WSASYSCALLFAILURE: return "System call failure.";
  case WSA_OPERATION_ABORTED: return "Overlapped operation aborted.";
  */
  }
}


#else /* unix, not WIN32 */

#define SOCKTYPE int
#define net_startup() ;
#define sock_errno errno
#define sock_strerror strerror
#define INVALID_SOCKET (-1)

#endif /* WIN32 */

/****************************************************************************/
/* more error handling */

/* error numbers passed around are normal system "errno" error numbers
 * (normally generated by socket operations), except when they have the
 * following values:
 */

enum {
  ERR_EOF      = MAXINT - 100,	/* reached end of file on socket */
  ERR_CLOSED   = MAXINT - 101,	/* attempted operation on closed socket */
  ERR_PROTOCOL = MAXINT - 102	/* some error in the received protocol */
};


/* return a string representation of an error number */

static const char * errorString (int n)
{
  switch (n) {
  case ERR_EOF: return "connection closed unexpectedly (\"end of file\")";
  case ERR_CLOSED: return "operation requested on a closed socket";
  case ERR_PROTOCOL: return "error in the received Lua-RPC protocol";
  default: return sock_strerror (n);
  }
}

/****************************************************************************/
/* exception handling using setjmp()/longjmp().
 *
 * do this:
 *
 *	TRY {
 *	  some_stuff();
 *	  THROW (error_code);
 *	  // if THROW is not called, you must call ENDTRY before the end of
 *	  // the TRY block (this includes before any `return' is called).
 *	  ENDTRY;
 *	}
 *	CATCH {
 *	  // *all* errors caught here, not just specific ones
 *	  there_is_an_error (ERRCODE);
 *	  if (dont_handle_here()) THROW (ERRCODE);
 *	}
 */

/* the exception stack. the top of the stack is the environment to longjmp()
 * to if there is a THROW.
 */

#define MAX_NESTED_TRYS 4

static jmp_buf exception_stack[MAX_NESTED_TRYS];
volatile static int exception_num_trys = 0;
volatile static int exception_errnum = 0;


/* you can call this when you have just entered or are about to leave a
 * Lua-RPC function from lua itself - this function resets the exception
 * stack, which is not used at all outside Lua-RPC.
 */

static void exception_init()
{
  exception_num_trys = 0;
  exception_errnum = 0;
}


/* throw an exception. this will jump to the most recent CATCH block. */

static void exception_throw (int n)
{
  MYASSERT (exception_num_trys > 0);
  exception_errnum = n;
  exception_num_trys--;
  longjmp (exception_stack[exception_num_trys],1);
}


#define THROW(errnum) exception_throw (errnum)

#define ERRCODE (exception_errnum)

#define TRY \
  MYASSERT (exception_num_trys < MAX_NESTED_TRYS); \
  exception_num_trys++; \
  if (setjmp (exception_stack[exception_num_trys-1]) == 0)

#define ENDTRY { \
  MYASSERT (exception_num_trys > 0); \
  exception_num_trys--; \
}

#define CATCH else

/****************************************************************************/
/* socket reading and writing stuff.
 * the socket functions throw exceptions if there are errors, so you must call
 * them from within a TRY block.
 */

/* socket structure */

struct _Socket {
  SOCKTYPE fd;			/* INVALID_SOCKET if socket is closed */
};
typedef struct _Socket Socket;


#define SOCK_VERIFY_OPEN \
  if (sock->fd == INVALID_SOCKET) THROW (ERR_CLOSED);


/* initialize a socket struct */

static void socket_init (Socket *sock)
{
  sock->fd = INVALID_SOCKET;
}


/* open a socket */

static void socket_open (Socket *sock)
{
  sock->fd = socket (PF_INET,SOCK_STREAM,IPPROTO_TCP);
  if (sock->fd == INVALID_SOCKET) THROW (sock_errno);
}


/* see if a socket is open */

static int socket_is_open (Socket *sock)
{
  return (sock->fd != INVALID_SOCKET);
}


/* close a socket */

static void socket_close (Socket *sock)
{
  if (sock->fd != INVALID_SOCKET) close (sock->fd);
  sock->fd = INVALID_SOCKET;
}


/* connect the socket to a host */

static void socket_connect (Socket *sock, u32 ip_address, u16 ip_port)
{
  struct sockaddr_in myname;
  SOCK_VERIFY_OPEN;
  myname.sin_family = AF_INET;
  myname.sin_port = htons (ip_port);
  myname.sin_addr.s_addr = htonl (ip_address);
  if (connect (sock->fd, (struct sockaddr *) &myname, sizeof (myname)) != 0)
    THROW (sock_errno);
}


/* bind the socket to a given address/port. the address can be INADDR_ANY. */

static void socket_bind (Socket *sock, u32 ip_address, u16 ip_port)
{
  struct sockaddr_in myname;
  SOCK_VERIFY_OPEN;
  myname.sin_family = AF_INET;
  myname.sin_port = htons (ip_port);
  myname.sin_addr.s_addr = htonl (ip_address);
  if (bind (sock->fd, (struct sockaddr *) &myname, sizeof (myname)) != 0)
    THROW (sock_errno);
}


/* listen for incoming connections, with up to `maxcon' connections
 * queued up.
 */

static void socket_listen (Socket *sock, int maxcon)
{
  SOCK_VERIFY_OPEN;
  if (listen (sock->fd,maxcon) != 0) THROW (sock_errno);
}


/* accept an incoming connection, initializing `asock' with the new connection.
 */

static void socket_accept (Socket *sock, Socket *asock)
{
  struct sockaddr_in clientname;
  size_t namesize;
  SOCK_VERIFY_OPEN;
  namesize = sizeof (clientname);
  asock->fd = accept (sock->fd, (struct sockaddr*) &clientname, &namesize);
  if (asock->fd == INVALID_SOCKET) THROW (sock_errno);
}


/* read from the socket into a buffer */

static void socket_read_buffer (Socket *sock, const u8 *buffer, int length)
{
  SOCK_VERIFY_OPEN;
  while (length > 0) {
    int n = read (sock->fd,(void*) buffer,length);
    if (n == 0) THROW (ERR_EOF);
    if (n < 0) THROW (sock_errno);
    buffer += n;
    length -= n;
  }
}


/* write a buffer to the socket */

static void socket_write_buffer (Socket *sock, const u8 *buffer, int length)
{
  int n;
  SOCK_VERIFY_OPEN;
  n = write (sock->fd,buffer,length);
  if (n != length) THROW (sock_errno);
}


/* read from the socket into a string buffer. */

static void socket_read_string (Socket *sock, const char *buffer, int length)
{
  socket_read_buffer (sock,(u8*) buffer,length);
}


/* write a string buffer to the socket */

static void socket_write_string (Socket *sock, const char *buffer, int length)
{
  socket_write_buffer (sock,(u8*) buffer,length);
}


/* read a u8 from the socket */

static u8 socket_read_u8 (Socket *sock)
{
  u8 b;
  SOCK_VERIFY_OPEN;
  socket_read_buffer (sock,&b,1);
  return b;
}


/* write a u8 to the socket */

static void socket_write_u8 (Socket *sock, u8 x)
{
  int n;
  SOCK_VERIFY_OPEN;
  n = write (sock->fd,&x,1);
  if (n != 1) THROW (sock_errno);
}


/* read a u32 from the socket */

static u32 socket_read_u32 (Socket *sock)
{
  u8 b[4];
  u32 i;
  SOCK_VERIFY_OPEN;
  socket_read_buffer (sock,b,4);
  i = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
  return i;
}


/* write a u32 to the socket */

static void socket_write_u32 (Socket *sock, u32 x)
{
  u8 b[4];
  int n;
  SOCK_VERIFY_OPEN;
  b[0] = x >> 24;
  b[1] = x >> 16;
  b[2] = x >> 8;
  b[3] = x;
  n = write (sock->fd,b,4);
  if (n != 4) THROW (sock_errno);
}


union DoubleBytes {
  double d;
  u8 b[8];
};


/* read a double from the socket */

static double socket_read_double (Socket *sock)
{
  union DoubleBytes double_bytes;
  SOCK_VERIFY_OPEN;
  /* @@@ handle endianness */
  socket_read_buffer (sock,double_bytes.b,8);
  return double_bytes.d;
}


/* write a double to the socket */

static void socket_write_double (Socket *sock, double x)
{
  int n;
  union DoubleBytes double_bytes;
  SOCK_VERIFY_OPEN;
  /* @@@ handle endianness */
  double_bytes.d = x;
  n = write (sock->fd,double_bytes.b,8);
  if (n != 8) THROW (sock_errno);
}


/* see if there is any data to read from a socket, without actually reading
 * it. return 1 if data is available, on 0 if not. if this is a listening
 * socket this returns 1 if a connection is available or 0 if not.
 */

static int socket_readable (Socket *sock)
{
  fd_set set;
  struct timeval tv;
  int ret;
  if (sock->fd == INVALID_SOCKET) return 0;
  FD_ZERO (&set);
  FD_SET (sock->fd,&set);
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  ret = select (sock->fd + 1,&set,0,0,&tv);
  return (ret > 0);
}

/****************************************************************************/
/* lua utility */

/* replacement for lua_error that resets the exception stack before leaving
 * Lua-RPC.
 */

void my_lua_error (lua_State *L, const char *errmsg)
{
  exception_init();
	printf(errmsg);
	lua_pushstring(L,errmsg);
  lua_error (L);
}


/* if the stack size is not `desired_n', trigger a lua runtime error. */

static int check_num_args (lua_State *L, int desired_n)
{
  int n = lua_gettop (L);		/* number of arguments on stack */
  if (n != desired_n) {
    char s[100];
    sprintf (s,"must have %d argument%c",desired_n,
	     (desired_n == 1) ? '\0' : 's');
    my_lua_error (L,s);
  }
  return n;
}

static int ismetatable_type (lua_State *L, int ud, const char *tname)
{
	if (lua_getmetatable(L, ud)) {  /* does it have a metatable? */
    lua_getfield(L, LUA_REGISTRYINDEX, tname);  /* get correct metatable */
    if (lua_rawequal(L, -1, -2)) {  /* does it have the correct mt? */
      lua_pop(L, 2);  /* remove both metatables */
      return 1;
    }
  }
	return 0;
}

/* check that a given stack value is a port number, and return its value. */

static int get_port_number (lua_State *L, int i)
{
  double port_d;
  int port;
  if (!lua_isnumber (L,i)) my_lua_error (L,"port number argument is bad");
  port_d = lua_tonumber (L,i);
  if (port_d < 0 || port_d > 0xffff)
    my_lua_error (L,"port number must be in the range 0..65535");
  port = (int) port_d;
  if (port_d != port) my_lua_error (L,"port number must be an integer");
  return port;
}

/****************************************************************************/
/* read and write lua variables to a socket.
 * these functions do little error handling of their own, but they call socket
 * functions which may throw exceptions, so calls to these functions must be
 * wrapped in a TRY block.
 */

enum {
  RPC_NIL=0,
  RPC_NUMBER,
  RPC_STRING,
  RPC_TABLE,
  RPC_TABLE_END
};

enum { RPC_PROTOCOL_VERSION = 2 };


/* prototypes */
static void write_variable (Socket *sock, lua_State *L, int var_index);
static int read_variable (Socket *sock, lua_State *L);


/* write a table at the given index in the stack. the index must be absolute
 * (i.e. positive).
 */

static void write_table (Socket *sock, lua_State *L, int table_index)
{
  lua_pushnil (L);	/* push first key */
  while (lua_next (L,table_index)) {
    /* next key and value were pushed on the stack */
    write_variable (sock,L,lua_gettop (L)-1);
    write_variable (sock,L,lua_gettop (L));
    /* remove value, keep key for next iteration */
    lua_pop (L,1);
  }
}


/* write a variable at the given index in the stack. the index must be absolute
 * (i.e. positive).
 */

static void write_variable (Socket *sock, lua_State *L, int var_index)
{
  int stack_at_start = lua_gettop (L);

  switch (lua_type (L,var_index)) {
  case LUA_TNUMBER:
    socket_write_u8 (sock,RPC_NUMBER);
    socket_write_double (sock,lua_tonumber (L,var_index));
    break;

  case LUA_TSTRING: {
    const char *s;
    u32 len;
    socket_write_u8 (sock,RPC_STRING);
    s = lua_tostring (L,var_index);
    len = lua_strlen (L,var_index);
    socket_write_u32 (sock,len);
    socket_write_string (sock,s,len);
    break;
  }

  case LUA_TTABLE:
    socket_write_u8 (sock,RPC_TABLE);
    write_table (sock,L,var_index);
    socket_write_u8 (sock,RPC_TABLE_END);
    break;

  case LUA_TNIL:
    socket_write_u8 (sock,RPC_NIL);
    break;

  case LUA_TFUNCTION:
    my_lua_error (L,"can't pass functions to a remote function");
    break;

  case LUA_TUSERDATA:
    my_lua_error (L,"can't pass user data to a remote function");
    break;
  }

  MYASSERT (lua_gettop (L) == stack_at_start);
}


/* read a table and push in onto the stack */

static void read_table (Socket *sock, lua_State *L)
{
  int table_index;
  lua_newtable (L);
  table_index = lua_gettop (L);
  for (;;) {
    if (!read_variable (sock,L)) return;
    read_variable (sock,L);
    lua_rawset (L,table_index);
  }
}


/* read a variable and push in onto the stack. this returns 1 if a "normal"
 * variable was read, or 0 if an end-table marker was read (in which case
 * nothing is pushed onto the stack).
 */

static int read_variable (Socket *sock, lua_State *L)
{
  u8 type = socket_read_u8 (sock);
  switch (type) {

  case RPC_NIL:
    lua_pushnil (L);
    break;

  case RPC_NUMBER:
    lua_pushnumber (L,socket_read_double (sock));
    break;

  case RPC_STRING: {
    u32 len = socket_read_u32 (sock);
    char *s = (char*) alloca (len+1);
    socket_read_string (sock,s,len);
    s[len] = 0;
    lua_pushlstring (L,s,len);
    break;
  }

  case RPC_TABLE:
    read_table (sock,L);
    break;

  case RPC_TABLE_END:
    return 0;

  default:
    THROW (ERR_PROTOCOL);	/* unknown type in request */
  }
  return 1;
}

/****************************************************************************/
/* client side handle and handle helper userdata objects.
 *
 * a handle userdata (handle to a RPC server) is a pointer to a Handle object.
 * a helper userdata is a pointer to a Helper object.
 *
 * helpers let us make expressions like:
 *    handle.funcname (a,b,c)
 * "handle.funcname" returns the helper object, which calls the remote
 * function.
 */

/* global error handling */
static int global_error_handler = LUA_NOREF;	/* function reference */


struct _Handle {
  int refcount;			/* delete the object when this goes to 0 */
  Socket sock;			/* the handle socket */
  int error_handler;		/* function reference */
  int async;			/* nonzero if async mode being used */
  int read_reply_count;		/* number of async call return values to read */
};
typedef struct _Handle Handle;


#define NUM_FUNCNAME_CHARS 4

struct _Helper {
  Handle *handle;			/* pointer to handle object */
  char funcname[NUM_FUNCNAME_CHARS];	/* name of the function */
};
typedef struct _Helper Helper; 


/* handle a client or server side error. NOTE: this function may or may not
 * return. the handle `h' may be 0.
 */

static void deal_with_error (lua_State *L, Handle *h, const char *error_string)
{
  if (global_error_handler !=  LUA_NOREF) {
    lua_getref (L,global_error_handler);
    lua_pushstring (L,error_string);
		lua_pcall (L,1,0,0);
  }
  else {
    my_lua_error (L,error_string);
  }
}


static Handle * handle_create (lua_State *L)
{
  Handle *h = (Handle *)lua_newuserdata(L, sizeof(Handle));
	luaL_getmetatable(L, "rpc.handle");
	lua_setmetatable(L, -2);
  h->refcount = 1;
  socket_open (&h->sock);
  h->error_handler = LUA_NOREF;
  h->async = 0;
  h->read_reply_count = 0;
  return h;
}


static void handle_ref (Handle *h)
{
  h->refcount++;
}


static void handle_deref (lua_State *L, Handle *h)
{
  h->refcount--;
  if (h->refcount <= 0) {
    socket_close (&h->sock);
    if (h->error_handler != LUA_NOREF) lua_unref (L,h->error_handler);
    free (h);
  }
}


static Helper * helper_create (lua_State *L, Handle *handle, const char *funcname)
{
	Helper *h = (Helper *)lua_newuserdata(L, sizeof (Helper) - NUM_FUNCNAME_CHARS + strlen(funcname) + 1);
	luaL_getmetatable(L, "rpc.helper");
	lua_setmetatable(L, -2);
  h->handle = handle;
  handle_ref (h->handle);
  strcpy (h->funcname,funcname);
  return h;
}


static void helper_destroy (lua_State *L, Helper *h)
{
  handle_deref (L,h->handle);
  free (h);
}


/* indexing a handle returns a helper */
static int handle_index (lua_State *L)
{
  const char *s;
	Helper *h;
	printf("Running Gettable!");
  MYASSERT (lua_gettop (L) == 2);
	MYASSERT (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.handle"));
  if (lua_type (L,2) != LUA_TSTRING)
    my_lua_error (L,"can't index a handle with a non-string");

  /* make a new helper object */
  s = lua_tostring (L,2);
	h = helper_create (L,(Handle*) lua_touserdata (L,1), s);

  /* return the helper object */
  return 1;
}


/* garbage collection for handles */

static int handle_gc (lua_State *L)
{
	printf("Running GC!");
  MYASSERT (lua_gettop (L) == 1);
	MYASSERT (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.handle"));
  handle_deref (L,(Handle*) lua_touserdata (L,1));
  return 0;
}

static int helper_function (lua_State *L)
{
  Helper *h;
  Socket *sock;
  MYASSERT (lua_gettop (L) >= 1);
  MYASSERT (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.helper"));
  exception_init();
	
  /* get helper object and its socket */
  h = (Helper*) lua_touserdata (L,1);
  sock = &h->handle->sock;

  TRY {
    int i,len,n;
    u32 nret,ret_code;

    /* first read out any pending return values for old async calls */
    for (; h->handle->read_reply_count > 0; h->handle->read_reply_count--) {
      ret_code = socket_read_u8 (sock);		/* return code */
      if (ret_code==0) {
	/* read return arguments, ignore everything we read */
	nret = socket_read_u32 (sock);
	for (i=0; i < ((int) nret); i++) read_variable (sock,L);
	lua_pop (L,nret);
      }
      else {
	/* read error and handle it */
	u32 code = socket_read_u32 (sock);
	u32 len = socket_read_u32 (sock);
	char *err_string = (char*) alloca (len+1);
	socket_read_string (sock,err_string,len);
	err_string[len] = 0;
	ENDTRY;
	deal_with_error (L,h->handle,err_string);
	return 0;
      }
    }

    /* write function name */
    len = strlen (h->funcname);
    socket_write_u32 (sock,len);
    socket_write_string (sock,h->funcname,len);

    /* write number of arguments */
    n = lua_gettop (L);
    socket_write_u32 (sock,n-1);

    /* write each argument */
    for (i=2; i<=n; i++) write_variable (sock,L,i);

    /* if we're in async mode, we're done */
    if (h->handle->async) {
      h->handle->read_reply_count++;
      ENDTRY;
      return 0;
    }

    /* read return code */
    ret_code = socket_read_u8 (sock);

    if (ret_code==0) {
      /* read return arguments */
      nret = socket_read_u32 (sock);
      for (i=0; i < ((int) nret); i++) read_variable (sock,L);
      ENDTRY;
      return nret;
    }
    else {
      /* read error and handle it */
      u32 code = socket_read_u32 (sock);
      u32 len = socket_read_u32 (sock);
      char *err_string = (char*) alloca (len+1);
      socket_read_string (sock,err_string,len);
      err_string[len] = 0;
      ENDTRY;
      deal_with_error (L,h->handle,err_string);
      return 0;
    }
  }
  CATCH {
    if (ERRCODE == ERR_CLOSED) {
      my_lua_error (L,"can't refer to a remote function after the handle has "
		    "been closed");
    }
    else {
      deal_with_error (L, h->handle, errorString (ERRCODE));
      socket_close (sock);
    }
    return 0;
  }
}


/* garbage collection for helpers */

static int helper_gc (lua_State *L)
{
	printf("Helper GC!");
  MYASSERT (lua_gettop (L) == 1);
  MYASSERT (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.helper"));
  helper_destroy (L,(Helper*) lua_touserdata (L,1));
  return 0;
}

/****************************************************************************/
/* server side handle userdata objects. */

struct _ServerHandle {
  Socket lsock;		/* listening socket, always valid if no error */
  Socket asock;		/* accepting socket, valid if connection established */
};
typedef struct _ServerHandle ServerHandle;


static ServerHandle * server_handle_create(lua_State *L)
{
  ServerHandle *h = (ServerHandle *)lua_newuserdata(L, sizeof(ServerHandle));
	luaL_getmetatable(L, "rpc.server_handle");
	lua_setmetatable(L, -2);
  socket_init (&h->lsock);
  socket_init (&h->asock);
  return h;
}


static void server_handle_shutdown (ServerHandle *h)
{
  socket_close (&h->lsock);
  socket_close (&h->asock);
}


static void server_handle_destroy (ServerHandle *h)
{
  server_handle_shutdown (h);
  free (h);
}

/****************************************************************************/
/* remote function calling (client side) */

/* RPC_open (ip_address, port)
 *     returns a handle to the new connection, or nil if there was an error.
 *     if there is an RPC error function defined, it will be called on error.
 */

static int RPC_open (lua_State *L)
{
  Handle *handle=0;
	
  exception_init();
  TRY {
    int ip_port;
    u32 ip_address;
    struct hostent *host;
    char header[5];

    check_num_args (L,2);
    if (!lua_isstring (L,1))
      my_lua_error (L,"first argument must be an ip address string");
    ip_port = get_port_number (L,2);

    host = gethostbyname (lua_tostring (L,1));
    if (!host) {
      deal_with_error (L,0,"could not resolve internet address");
      lua_pushnil (L);
      ENDTRY;
      return 1;
    }

    if (host->h_addrtype != AF_INET || host->h_length != 4) {
      deal_with_error (L,0,"not an internet IPv4 address");
      lua_pushnil (L);
      ENDTRY;
      return 1;
    }
    ip_address = ntohl ( *((u32*)host->h_addr_list[0]) );

    /* make handle */
    handle = handle_create(L);

    /* connect the socket to the target server */
    socket_connect (&handle->sock,ip_address,(u16) ip_port);

    /* write the protocol header */
    header[0] = 'L';
    header[1] = 'R';
    header[2] = 'P';
    header[3] = 'C';
    header[4] = RPC_PROTOCOL_VERSION;
    socket_write_string (&handle->sock,header,sizeof(header));
		
    ENDTRY;
    return 1;
  }
  CATCH {
    if (handle) handle_deref (L,handle);
    deal_with_error (L, 0, errorString (ERRCODE));
    lua_pushnil (L);
    return 1;
  }
}


/* RPC_close (handle)
 *     this closes the socket, but does not free the handle object. that's
 *     because the handle will still be in the user's name space and might be
 *     referred to again. we'll let garbage collection free the object.
 *     it's a lua runtime error to refer to a socket after it has been closed.
 */

static int RPC_close (lua_State *L)
{
  check_num_args (L,1);

  if (lua_isuserdata (L,1)) {
    if (ismetatable_type(L, 1, "rpc.handle")) {
      Handle *handle = (Handle*) lua_touserdata (L,1);
      socket_close (&handle->sock);
      return 0;
    }
    if (ismetatable_type(L, 1, "rpc.server_handle")) {
      ServerHandle *handle = (ServerHandle*) lua_touserdata (L,1);
      server_handle_shutdown (handle);
      return 0;
    }
  }

  my_lua_error (L,"argument must be an RPC handle");
  return 0;
}



/* RPC_async (handle,)
 *     this sets a handle's asynchronous calling mode (0/nil=off, other=on).
 *     (this is for the client only).
 */

static int RPC_async (lua_State *L)
{
  Handle *handle;
  check_num_args (L,2);

  if (!lua_isuserdata (L,1) || !ismetatable_type(L, 1, "rpc.handle"))
    my_lua_error (L,"first argument must be an RPC client handle");
  handle = (Handle*) lua_touserdata (L,1);
  if (lua_isnil (L,2) || (lua_isnumber (L,2) && lua_tonumber (L,2) == 0))
    handle->async = 0;
  else
    handle->async = 1;

  return 0;
}

/****************************************************************************/
/* lua remote function server */

/* a temporary replacement for the _ERRORMESSAGE function, used to catch
 * server side lua errors.
 */

static char tmp_errormessage_buffer[200];

static int server_err_handler (lua_State *L)
{
  if (lua_gettop (L) >= 1) {
    strncpy (tmp_errormessage_buffer, lua_tostring (L,1),
	     sizeof (tmp_errormessage_buffer));
    tmp_errormessage_buffer [sizeof (tmp_errormessage_buffer)-1] = 0;
  }
  return 0;
}


/* read function call data and execute the function. this function empties the
 * stack on entry and exit. this redefines _ERRORMESSAGE to catch errors around
 * the function call.
 */

static void read_function_call (Socket *sock, lua_State *L)
{
  int i,stackpos,good_function,nargs;
  u32 len;
  char *funcname;

  /* read function name */
  len = socket_read_u32 (sock);	/* function name string length */
	
  funcname = (char*) alloca (len+1);
  socket_read_string (sock,funcname,len);
  funcname[len] = 0;

  /* get function */
  stackpos = lua_gettop (L);
  lua_getglobal (L,funcname);
  good_function = lua_isfunction (L,-1);

  /* read number of arguments */
  nargs = socket_read_u32 (sock);

  /* read in each argument, leave it on the stack */
  for (i=0; i<nargs; i++) read_variable (sock,L);

  /* call the function */
  if (good_function) {
    int nret,error_code;
				
    lua_pushcfunction (L,server_err_handler);
    error_code = lua_pcall (L,nargs,LUA_MULTRET, -1);

    /* handle errors */
    if (error_code || tmp_errormessage_buffer[0]) {
      int len = strlen (tmp_errormessage_buffer);
      socket_write_u8 (sock,1);
      socket_write_u32 (sock,error_code);
      socket_write_u32 (sock,len);
      socket_write_string (sock,tmp_errormessage_buffer,len);
    }
    else {
      /* pass the return values back to the caller */
      socket_write_u8 (sock,0);
      nret = lua_gettop (L) - stackpos;
      socket_write_u32 (sock,nret);
      for (i=0; i<nret; i++) write_variable (sock,L,stackpos+1+i);
    }
  }
  else {
    /* bad function */
    const char *msg = "undefined function: ";
    int errlen = strlen (msg) + len;
    socket_write_u8 (sock,1);
    socket_write_u32 (sock,LUA_ERRRUN);
    socket_write_u32 (sock,errlen);
    socket_write_string (sock,msg,strlen(msg));
    socket_write_string (sock,funcname,len);
  }

  /* empty the stack */
  lua_settop (L,0);
}


static ServerHandle *RPC_listen_helper (lua_State *L)
{
	ServerHandle *handle = 0;
  exception_init();

  TRY {
    int port;

    check_num_args (L,1);
    port = get_port_number (L,1);

    /* make server handle */
    handle = server_handle_create(L);

    /* make listening socket */
    socket_open (&handle->lsock);
    socket_bind (&handle->lsock,INADDR_ANY,(u16) port);
    socket_listen (&handle->lsock,MAXCON);
		
    ENDTRY;
    return handle;
  }
  CATCH {
    if (handle) server_handle_destroy (handle);
    deal_with_error (L, 0, errorString (ERRCODE));
    return 0;
  }
}


/* RPC_listen (port) --> server_handle */

static int RPC_listen (lua_State *L)
{
  ServerHandle *handle;
	handle = RPC_listen_helper (L);
  return 1;
}


/* RPC_peek (server_handle) --> 0 or 1 */

static int RPC_peek (lua_State *L)
{
  ServerHandle *handle;
  check_num_args (L,1);
  if (!(lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.server_handle")))
    my_lua_error (L,"argument must be an RPC server handle");

  handle = (ServerHandle*) lua_touserdata (L,1);

  /* if accepting socket is open, see if there is any data to read */
  if (socket_is_open (&handle->asock)) {
    if (socket_readable (&handle->asock)) lua_pushnumber (L,1);
    else lua_pushnil (L);
    return 1;
  }

  /* otherwise, see if there is a new connection on the listening socket */
  if (socket_is_open (&handle->lsock)) {
    if (socket_readable (&handle->lsock)) lua_pushnumber (L,1);
    else lua_pushnil (L);
    return 1;
  }

  lua_pushnumber (L,0);
  return 1;
}


static void RPC_dispatch_helper (lua_State *L, ServerHandle *handle)
{
  exception_init();
  TRY {
    /* if accepting socket is open, read function calls */
    if (socket_is_open (&handle->asock)) {
      TRY {
	read_function_call (&handle->asock,L);
	ENDTRY;
      }
      CATCH {
	/* if the client has closed the connection, close our side
	 * gracefully too.
	 */
	socket_close (&handle->asock);
	if (ERRCODE != ERR_EOF && ERRCODE != ERR_PROTOCOL) THROW (ERRCODE);
      }
    }
    else {
      /* if accepting socket is not open, accept a new connection from the
       * listening socket.
       */
      char header[5];
      socket_accept (&handle->lsock, &handle->asock);

      /* check that the header is ok */
      socket_read_string (&handle->asock,header,sizeof(header));
      if (header[0] != 'L' ||
	  header[1] != 'R' ||
	  header[2] != 'P' ||
	  header[3] != 'C' ||
	  header[4] != RPC_PROTOCOL_VERSION) {
	/* bad remote function call header, close the connection */
	socket_close (&handle->asock);
	ENDTRY;
	return;
      }
    }

    ENDTRY;
  }
  CATCH {
    server_handle_shutdown (handle);
    deal_with_error (L, 0, errorString (ERRCODE));
  }
}


/* RPC_dispatch (server_handle) */

static int RPC_dispatch (lua_State *L)
{
  ServerHandle *handle;
  check_num_args (L,1);
  if (!(lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.server_handle")))
    my_lua_error (L,"argument must be an RPC server handle");
  handle = (ServerHandle*) lua_touserdata (L,1);

  RPC_dispatch_helper (L,handle);
  return 0;
}


/* lrf_server (port) */

static int RPC_server (lua_State *L)
{
  ServerHandle *handle = RPC_listen_helper (L);
	printf("RPC Server!");
  while (socket_is_open (&handle->lsock)) {
    RPC_dispatch_helper (L,handle);
  }
  server_handle_destroy (handle);
  return 0;
}


/* garbage collection for server handles */

static int server_handle_gc (lua_State *L)
{
	printf("Server Handle GC!");
  MYASSERT (lua_gettop (L) == 1);
  MYASSERT (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.server_handle"));
  server_handle_destroy ((ServerHandle*) lua_touserdata (L,1));
  return 0;
}

/****************************************************************************/
/* more error handling stuff */

/* RPC_on_error ([handle,] error_handler)
 */

static int RPC_on_error (lua_State *L)
{
  check_num_args (L,1);

  if (global_error_handler !=  LUA_NOREF) lua_unref (L,global_error_handler);
  global_error_handler = LUA_NOREF;

  if (lua_isfunction (L,1)) {
    global_error_handler = lua_ref (L,1);
  }
  else if (lua_isnil (L,1)) {
  }
  else my_lua_error (L,"bad arguments");

  /* @@@ add option for handle */
  /* Handle *h = (Handle*) lua_touserdata (L,1); */
  /* if (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.handle")); */

  return 0;
}

/****************************************************************************/
/* register RPC functions */

/* debugging function */

static int garbage_collect (lua_State *L)
{
	printf("Collecting!");
  lua_gc (L,LUA_GCCOLLECT,0);
  return 0;
}

static const luaL_reg rpc_handle[] =
{
	{ "__index",	handle_index		},
	{ NULL,		NULL		}
};

static const luaL_reg rpc_helper[] =
{
	{ "__call",	helper_function		},
	{ NULL,		NULL		}
};

static const luaL_reg rpc_server_handle[] =
{
	{ NULL,		NULL		}
};


LUALIB_API int luaopen_rpc(lua_State *L)
{
 	static int started = 0;
  if (started) panic ("luaopen_rpc() called more than once");
  started = 1;

  net_startup();
  lua_register (L,"RPC_open",RPC_open);
  lua_register (L,"RPC_close",RPC_close);
  lua_register (L,"RPC_server",RPC_server);
  lua_register (L,"RPC_on_error",RPC_on_error);
  lua_register (L,"RPC_listen",RPC_listen);
  lua_register (L,"RPC_peek",RPC_peek);
  lua_register (L,"RPC_dispatch",RPC_dispatch);
  lua_register (L,"RPC_async",RPC_async);

	luaL_newmetatable(L, "rpc.helper");
	luaL_openlib(L,NULL,rpc_helper,0);
	
	
	luaL_newmetatable(L, "rpc.handle");
	luaL_openlib(L,NULL,rpc_handle,0);
	
	
	luaL_newmetatable(L, "rpc.server_handle");
 	luaL_openlib(L,NULL,rpc_server_handle,0);

  if (sizeof(double) != 8)
    debug ("internal error: sizeof(double) != 8");

 	return 1;
}