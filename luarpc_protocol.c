/*****************************************************************************
* Lua-RPC library, Copyright (C) 2001 Russell L. Smith. All rights reserved. *
*   Email: russ@q12.org   Web: www.q12.org                                   *
* For documentation, see http://www.q12.org/lua. For the license agreement,  *
* see the file LICENSE that comes with this distribution.                    *
*****************************************************************************/

// Modifications by James Snyder - jbsnyder@fanplastic.org
//  - more generic backend interface to accomodate different link types
//  - integration with eLua (including support for rotables)
//  - extensions of remote global table as local table metaphor
//    - methods to allow remote assignment, getting remote values
//    - accessing and calling types nested multiple levels deep on tables now works
//  - port from Lua 4.x to 5.x

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef __MINGW32__
void *alloca(size_t);
#else
#include <alloca.h>
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#if !defined( LUA_CROSS_COMPILER ) && !defined( LUARPC_STANDALONE )
#include "platform.h"
#endif

#include "platform_conf.h"

#ifdef LUA_OPTIMIZE_MEMORY
#include "lrotable.h"
#endif

#include "luarpc_rpc.h"
#include "luarpc_protocol.h"


#ifdef BUILD_RPC
/*
// Support for Compiling with & without rotables 
#ifdef LUA_OPTIMIZE_MEMORY
#define LUA_ISCALLABLE( state, idx ) ( lua_isfunction( state, idx ) || lua_islightfunction( state, idx ) )
#else
#define LUA_ISCALLABLE( state, idx ) lua_isfunction( state, idx )
#endif*/

// Prototypes for Local Functions  
LUALIB_API int luaopen_rpc( lua_State *L );
Handle *handle_create( lua_State *L );


struct exception_context the_exception_context[ 1 ];



// Lua Types
enum {
  RPC_NIL=0,
  RPC_NUMBER,
  RPC_BOOLEAN,
  RPC_STRING,
  RPC_TABLE,
  RPC_TABLE_END,
  RPC_FUNCTION,
  RPC_FUNCTION_END,
  RPC_REMOTE
};

// RPC Commands
enum
{
  RPC_CMD_CALL = 1,
  RPC_CMD_GET,
  RPC_CMD_CON,
  RPC_CMD_NEWINDEX
};

// RPC Status Codes
enum
{
  RPC_READY = 64,
  RPC_UNSUPPORTED_CMD,
  RPC_DONE
};

enum { RPC_PROTOCOL_VERSION = 3 };


// return a string representation of an error number 

const char * error_string( int n )
{
  switch (n) {
    case ERR_EOF: return "connection closed unexpectedly";
    case ERR_CLOSED: return "operation requested on closed transport";
    case ERR_PROTOCOL: return "error in the received protocol";
    case ERR_COMMAND: return "undefined command";
    case ERR_NODATA: return "no data received when attempting to read";
    case ERR_HEADER: return "header exchanged failed";
    case ERR_LONGFNAME: return "function name too long";
    case ERR_TIMEOUT: return "timeout";
    default: return transport_strerror( n );
  }
}


// **************************************************************************
// transport layer generics

// read arbitrary length from the transport into a string buffer. 
void transport_read_string( Transport *tpt, char *buffer, int length )
{
  transport_read_buffer( tpt, ( u8 * )buffer, length );
}


// write arbitrary length string buffer to the transport 
void transport_write_string( Transport *tpt, const char *buffer, int length )
{
  transport_write_buffer( tpt, ( u8 * )buffer, length );
}


// read a u8 from the transport 
u8 transport_read_u8( Transport *tpt )
{
  u8 b;
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  transport_read_buffer( tpt, &b, 1 );
  return b;
}


// write a u8 to the transport 
void transport_write_u8( Transport *tpt, u8 x )
{
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  transport_write_buffer( tpt, &x, 1 );
}

static void swap_bytes( uint8_t *number, size_t numbersize )
{
  int i;
  for ( i = 0 ; i < numbersize / 2 ; i ++ )
  {
    uint8_t temp = number[ i ];
    number[ i ] = number[ numbersize - 1 - i ];
    number[ numbersize - 1 - i ] = temp;
  }
}

union u32_bytes {
  uint32_t i;
  uint8_t  b[ 4 ];
};

// read a u32 from the transport 
u32 transport_read_u32( Transport *tpt )
{
  union u32_bytes ub;
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  transport_read_buffer ( tpt, ub.b, 4 );
  if( tpt->net_little != tpt->loc_little )
    swap_bytes( ( uint8_t * )ub.b, 4 );
  return ub.i;
}


// write a u32 to the transport 
void transport_write_u32( Transport *tpt, u32 x )
{
  union u32_bytes ub;
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  ub.i = ( uint32_t )x;
  if( tpt->net_little != tpt->loc_little )
    swap_bytes( ( uint8_t * )ub.b, 4 );
  transport_write_buffer( tpt, ub.b, 4 );
}

// read a lua number from the transport 
static lua_Number transport_read_number( Transport *tpt )
{
  lua_Number x;
  u8 b[ tpt->lnum_bytes ];
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  transport_read_buffer ( tpt, b, tpt->lnum_bytes );
  
  if( tpt->net_little != tpt->loc_little )
    swap_bytes( ( uint8_t * )b, tpt->lnum_bytes );
  
  if( tpt->net_intnum != tpt->loc_intnum ) // if we differ on num types, use int
  {
    switch( tpt->lnum_bytes ) // read integer types
    {
      case 1: {
        int8_t y = *( int8_t * )b;
        x = ( lua_Number )y;
      } break;
       case 2: {
        int16_t y = *( int16_t * )b;
        x = ( lua_Number )y;
      } break;
      case 4: {
        int32_t y = *( int32_t * )b;
        x = ( lua_Number )y;
      } break;
      case 8: {
        int64_t y = *( int64_t * )b;
        x = ( lua_Number )y;
      } break;
      default: lua_assert( 0 );
    }
  }
  else
    x = ( lua_Number ) *( lua_Number * )b; // if types match, use native type
    
  return x;
}


// write a lua number to the transport 
static void transport_write_number( Transport *tpt, lua_Number x )
{
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
   
  if( tpt->net_intnum )
  {
    switch( tpt->lnum_bytes )
    {
      case 1: {
        int8_t y = ( int8_t )x;
        transport_write_buffer( tpt, ( u8 * )&y, 1 );
      } break;
      case 2: {
        int16_t y = ( int16_t )x;
        if( tpt->net_little != tpt->loc_little )
          swap_bytes( ( uint8_t * )&y, 2 );
        transport_write_buffer( tpt, ( u8 * )&y, 2 );
      } break;
      case 4: {
        int32_t y = ( int32_t )x;
        if( tpt->net_little != tpt->loc_little )
          swap_bytes( ( uint8_t * )&y, 4 );
        transport_write_buffer( tpt,( u8 * )&y, 4 );
      } break;
      case 8: {
        int64_t y = ( int64_t )x;
        if( tpt->net_little != tpt->loc_little )
          swap_bytes( ( uint8_t * )&y, 8 );
        transport_write_buffer( tpt, ( u8 * )&y, 8 );
      } break;
      default: lua_assert(0);
    }
  }
  else
  {
    if( tpt->net_little != tpt->loc_little )
       swap_bytes( ( uint8_t * )&x, 8 );
    transport_write_buffer( tpt, ( u8 * )&x, 8 );
  }
}



// **************************************************************************
// lua utilities


int check_num_args( lua_State *L, int desired_n )
{
  int n = lua_gettop( L );   // number of arguments on stack
  if ( n != desired_n )
  {
    return luaL_error( L, "must have %d arg%c", desired_n,
       ( desired_n == 1 ) ? '\0' : 's' );
  }
  return n;
}

int ismetatable_type( lua_State *L, int ud, const char *tname )
{
  if( lua_getmetatable( L, ud ) ) {  // does it have a metatable?
    lua_getfield( L, LUA_REGISTRYINDEX, tname );  // get correct metatable
    if( lua_rawequal( L, -1, -2 ) ) {  // does it have the correct mt?
      lua_pop( L, 2 );  // remove both metatables
      return 1;
    }
  }
  return 0;
}



/****************************************************************************/
// read and write lua variables to a transport.
//   these functions do little error handling of their own, but they call transport
//   functions which may throw exceptions, so calls to these functions must be
//   wrapped in a Try block.

static void write_variable( Transport *tpt, lua_State *L, int var_index );
static int read_variable( Transport *tpt, lua_State *L );

// write a table at the given index in the stack. the index must be absolute
// (i.e. positive).
// @@@ circular table references will cause stack overflow!
static void write_table( Transport *tpt, lua_State *L, int table_index )
{
  lua_pushnil( L );  // push first key
  while ( lua_next( L, table_index ) ) 
  {
    // next key and value were pushed on the stack 
    write_variable( tpt, L, lua_gettop( L ) - 1 );
    write_variable( tpt, L, lua_gettop( L ) );
    
    // remove value, keep key for next iteration 
    lua_pop( L, 1 );
  }
}

static int writer( lua_State *L, const void* b, size_t size, void* B ) {
  (void)L;
  luaL_addlstring((luaL_Buffer*) B, (const char *)b, size);
  return 0;
}

#if defined( LUA_CROSS_COMPILER )  && !defined( LUARPC_STANDALONE )
#include "lundump.h"
#include "ldo.h"

// Dump bytecode representation of function onto stack and send. This
// implementation uses eLua's crosscompile dump to match match the
// bytecode representation to the client/server negotiated format.
static void write_function( Transport *tpt, lua_State *L, int var_index )
{
  TValue *o;
  luaL_Buffer b;
  DumpTargetInfo target;
  
  target.little_endian=tpt->net_little;
  target.sizeof_int=sizeof(int);
  target.sizeof_strsize_t=sizeof(strsize_t);
  target.sizeof_lua_Number=tpt->lnum_bytes;
  target.lua_Number_integral=tpt->net_intnum;
  target.is_arm_fpa=0;
  
  // push function onto stack, serialize to string 
  lua_pushvalue( L, var_index );
  luaL_buffinit( L, &b );
  lua_lock(L);
  o = L->top - 1;
  luaU_dump_crosscompile(L,clvalue(o)->l.p,writer,&b,0,target);
  lua_unlock(L);
  
  // put string representation on stack and send it
  luaL_pushresult( &b );
  write_variable( tpt, L, lua_gettop( L ) );
  
  // Remove function & dumped string from stack
  lua_pop( L, 2 );
}
#else
static void write_function( Transport *tpt, lua_State *L, int var_index )
{
  luaL_Buffer b;
  
  // push function onto stack, serialize to string 
  lua_pushvalue( L, var_index );
  luaL_buffinit( L, &b );
  lua_dump(L, writer, &b);
  
  // put string representation on stack and send it
  luaL_pushresult( &b );
  write_variable( tpt, L, lua_gettop( L ) );
  
  // Remove function & dumped string from stack
  lua_pop( L, 2 );
}
#endif

static void helper_remote_index( Helper *helper );

// write a variable at the given index in the stack. the index must be absolute
// (i.e. positive).

static void write_variable( Transport *tpt, lua_State *L, int var_index )
{
  int stack_at_start = lua_gettop( L );
  
  switch( lua_type( L, var_index ) )
  {
    case LUA_TNUMBER:
      transport_write_u8( tpt, RPC_NUMBER );
      transport_write_number( tpt, lua_tonumber( L, var_index ) );
      break;

    case LUA_TSTRING:
    {
      const char *s;
      u32 len;
      transport_write_u8( tpt, RPC_STRING );
      s = lua_tostring( L, var_index );
      len = lua_strlen( L, var_index );
      transport_write_u32( tpt, len );
      transport_write_string( tpt, s, len );
      break;
    }

    case LUA_TTABLE:
      transport_write_u8( tpt, RPC_TABLE );
      write_table( tpt, L, var_index );
      transport_write_u8( tpt, RPC_TABLE_END );
      break;

    case LUA_TNIL:
      transport_write_u8( tpt, RPC_NIL );
      break;

    case LUA_TBOOLEAN:
      transport_write_u8( tpt,RPC_BOOLEAN );
      transport_write_u8( tpt, ( u8 )lua_toboolean( L, var_index ) );
      break;

    case LUA_TFUNCTION:
      transport_write_u8( tpt, RPC_FUNCTION );
      write_function( tpt, L, var_index );
      transport_write_u8( tpt, RPC_FUNCTION_END );
      break;

    case LUA_TUSERDATA:
      if( lua_isuserdata( L, var_index ) && ismetatable_type( L, var_index, "rpc.helper" ) )
      {
        transport_write_u8( tpt, RPC_REMOTE );
        helper_remote_index( ( Helper * )lua_touserdata( L, var_index ) );        
      } else
        luaL_error( L, "userdata transmission unsupported" );
      break;

    case LUA_TTHREAD:
      luaL_error( L, "thread transmission unsupported" );
      break;

    case LUA_TLIGHTUSERDATA:
      luaL_error( L, "light userdata transmission unsupported" );
      break;
  }
  MYASSERT( lua_gettop( L ) == stack_at_start );
}


// read a table and push in onto the stack 
static void read_table( Transport *tpt, lua_State *L )
{
  int table_index;
  lua_newtable( L );
  table_index = lua_gettop( L );
  for ( ;; ) 
  {
    if( !read_variable( tpt, L ) )
      return;
    read_variable( tpt, L );
    lua_rawset( L, table_index );
  }
}

// read function and load
static void read_function( Transport *tpt, lua_State *L )
{
  const char *b;
  size_t len;
  
  for( ;; )
  {
    if( !read_variable( tpt, L ) )
      return;

    b = luaL_checklstring( L, -1, &len );
    luaL_loadbuffer( L, b, len, b );
    lua_insert( L, -2 );
    lua_pop( L, 1 );
  }
}

static void read_index( Transport *tpt, lua_State *L )
{
  u32 len;
  char *funcname;
  char *token = NULL;
  
  len = transport_read_u32( tpt ); // variable name length
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;
  
  token = strtok( funcname, "." );
  lua_getglobal( L, token );
  token = strtok( NULL, "." );
  while( token != NULL )
  {
    lua_getfield( L, -1, token );
    lua_remove( L, -2 );
    token = strtok( NULL, "." );
  }
}


// read a variable and push in onto the stack. this returns 1 if a "normal"
// variable was read, or 0 if an end-table or end-function marker was read (in which case
// nothing is pushed onto the stack).
static int read_variable( Transport *tpt, lua_State *L )
{
  struct exception e;
  u8 type = transport_read_u8( tpt );

  switch( type )
  {
    case RPC_NIL:
      lua_pushnil( L );
      break;

    case RPC_BOOLEAN:
      lua_pushboolean( L, transport_read_u8( tpt ) );
      break;

    case RPC_NUMBER:
      lua_pushnumber( L, transport_read_number( tpt ) );
      break;

    case RPC_STRING:
    {
      u32 len = transport_read_u32( tpt );
      char *s = ( char * )alloca( len + 1 );
      transport_read_string( tpt, s, len );
      s[ len ] = 0;
      lua_pushlstring( L, s, len );
      break;
    }

    case RPC_TABLE:
      read_table( tpt, L );
      break;

    case RPC_TABLE_END:
      return 0;

    case RPC_FUNCTION:
      read_function( tpt, L );
      break;
    
    case RPC_FUNCTION_END:
      return 0;

    case RPC_REMOTE:
      read_index( tpt, L );
      break;

    default:
      e.errnum = type;
      e.type = fatal;
      Throw( e );
  }
  return 1;
}


// **************************************************************************
// rpc utilities

// functions for sending and receving headers 

void client_negotiate( Transport *tpt )
{
  struct exception e;
  char header[ 8 ];
  int x = 1;

  // default client configuration
  tpt->loc_little = ( char )*( char * )&x;
  tpt->lnum_bytes = ( char )sizeof( lua_Number );
  tpt->loc_intnum = ( char )( ( ( lua_Number )0.5 ) == 0 );
  transport_write_u8( tpt, RPC_CMD_CON );

  // write the protocol header 
  header[0] = 'L';
  header[1] = 'R';
  header[2] = 'P';
  header[3] = 'C';
  header[4] = RPC_PROTOCOL_VERSION;
  header[5] = tpt->loc_little;
  header[6] = tpt->lnum_bytes;
  header[7] = tpt->loc_intnum;
  //  printf("write version\n");
  transport_write_string( tpt, header, sizeof( header ) );
  transport_flush(tpt);
  //  printf("write version ok\n");
  
  // read server's response
  //  printf("read version\n");
  transport_read_string( tpt, header, sizeof( header ) );
  if( header[0] != 'L' ||
      header[1] != 'R' ||
      header[2] != 'P' ||
      header[3] != 'C' ||
      header[4] != RPC_PROTOCOL_VERSION )
  {
    e.errnum = ERR_HEADER;
    e.type = nonfatal;
    Throw( e );
  }
      printf("read version ok\n");
  // write configuration from response
  tpt->net_little = header[5];
  tpt->lnum_bytes = header[6];
  tpt->net_intnum = header[7];
}

void server_negotiate( Transport *tpt )
{
  struct exception e;
  char header[ 8 ];
  int x = 1;

  if( transport_read_u8( tpt ) != RPC_CMD_CON ){
    e.errnum = ERR_HEADER;
    e.type = nonfatal;
    Throw( e );
  }
  
  // default sever configuration
  tpt->net_little = tpt->loc_little = ( char )*( char * )&x;
  tpt->lnum_bytes = ( char )sizeof( lua_Number );
  tpt->net_intnum = tpt->loc_intnum = ( char )( ( ( lua_Number )0.5 ) == 0 );
  
  // read and check header from client
  //  printf("read version\n");
  transport_read_string( tpt, header, sizeof( header ) );
  if( header[0] != 'L' ||
      header[1] != 'R' ||
      header[2] != 'P' ||
      header[3] != 'C' ||
      header[4] != RPC_PROTOCOL_VERSION )
  {
    e.errnum = ERR_HEADER;
    e.type = nonfatal;
    Throw( e );
  }
  //printf("read version ok\n");
  // check if endianness differs, if so use big endian order  
  if( header[ 5 ] != tpt->loc_little )
    header[ 5 ] = tpt->net_little = 0;
    
  // set number precision to lowest common denominator 
  if( header[ 6 ] > tpt->lnum_bytes )
    header[ 6 ] = tpt->lnum_bytes;
  if( header[ 6 ] < tpt->lnum_bytes )
    tpt->lnum_bytes = header[ 6 ];
  
  // if lua_Number is integer on either side, use integer 
  if( header[ 7 ] != tpt->loc_intnum )
    header[ 7 ] = tpt->net_intnum = 1;
  
  //printf("write version\n");
  // send reconciled configuration to client
  transport_write_string( tpt, header, sizeof( header ) );
  transport_flush(tpt);
  //printf("write version ok\n");
}


static int generic_catch_handler(lua_State *L, Transport* trans, struct exception e )
{
  deal_with_error( L, error_string( e.errnum ) );
  switch( e.type )
  {
    case nonfatal:
      printf("GEN NON FATAL");
      lua_pushnil( L );
      return 1;
      break;
    case fatal:
      printf("GEN FATAL");
      transport_delete( trans );
      break;
    default: lua_assert( 0 );
  }
  return 0;
}

// **************************************************************************
// client side handle and handle helper userdata objects.
//
//  a handle userdata (handle to a RPC server) is a pointer to a Handle object.
//  a helper userdata is a pointer to a Helper object.
//
//  helpers let us make expressions like:
//     handle.funcname (a,b,c)
//  "handle.funcname" returns the helper object, which calls the remote
//  function.

// global error default (no handler) 
int global_error_handler = LUA_NOREF;

// handle a client or server side error. NOTE: this function may or may not
// return. the handle `h' may be 0.
void deal_with_error(lua_State *L, const char *error_string)
{ 
  if( global_error_handler !=  LUA_NOREF )
  {
    lua_getref( L, global_error_handler );
    lua_pushstring( L, error_string );
    lua_pcall( L, 1, 0, 0 );
  }
  else
    luaL_error( L, error_string );
}

Handle *handle_create( lua_State *L )
{
  Handle *h = ( Handle * )lua_newuserdata( L, sizeof( Handle ) );
  luaL_getmetatable( L, "rpc.handle" );
  lua_setmetatable( L, -2 );
  h->error_handler = LUA_NOREF;
  h->async = 0;
  h->read_reply_count = 0;
  return h;
}

static Helper *helper_create( lua_State *L, Handle *handle, const char *funcname )
{
  Helper *h = ( Helper * )lua_newuserdata( L, sizeof( Helper ) );
  luaL_getmetatable( L, "rpc.helper" );
  lua_setmetatable( L, -2 );
  
  lua_pushvalue( L, 1 ); // push parent handle
  h->pref = luaL_ref( L, LUA_REGISTRYINDEX ); // put ref into struct
  h->handle = handle;
  h->parent = NULL;
  h->nparents = 0;
  strncpy( h->funcname, funcname, NUM_FUNCNAME_CHARS );
  return h;
}


// indexing a handle returns a helper 
int handle_index (lua_State *L)
{
  const char *s;
  
  check_num_args( L, 2 );
  MYASSERT( lua_isuserdata( L, 1 ) && ismetatable_type( L, 1, "rpc.handle" ) );

  if( lua_type( L, 2 ) != LUA_TSTRING )
    return luaL_error( L, "can't index a handle with a non-string" );
  s = lua_tostring( L, 2 );
  if ( strlen( s ) > NUM_FUNCNAME_CHARS - 1 )
    return luaL_error( L, error_string( ERR_LONGFNAME ) );
    
  helper_create( L, ( Handle * )lua_touserdata( L, 1 ), s );

  // return the helper object 
  return 1;
}

int helper_newindex( lua_State *L );

// indexing a handle returns a helper
int handle_newindex( lua_State *L )
{
  const char *s;

  check_num_args( L, 3 );
  MYASSERT( lua_isuserdata( L, 1 ) && ismetatable_type( L, 1, "rpc.handle" ) );

  if( lua_type( L, 2 ) != LUA_TSTRING )
    return luaL_error( L, "can't index handle with a non-string" );
  s = lua_tostring( L, 2 );
  if ( strlen( s ) > NUM_FUNCNAME_CHARS - 1 )
    return luaL_error( L, error_string( ERR_LONGFNAME ) );
  
  helper_create( L, ( Handle * )lua_touserdata( L, 1 ), "" );
  lua_replace(L, 1);

  helper_newindex( L );

  return 0;
}

// replays series of indexes to remote side as a string
static void helper_remote_index( Helper *helper )
{
  int i, len;
  Helper **hstack;
  Transport *tpt = &helper->handle->tpt;
  
  // get length of name & make stack of helpers
  len = strlen( helper->funcname );
  if( helper->nparents > 0 ) // If helper has parents, build string to remote index
  {
    hstack = ( Helper ** )alloca( sizeof( Helper * ) * helper->nparents );
    hstack[ helper->nparents - 1 ] = helper->parent;
    len += strlen( hstack[ helper->nparents - 1 ]->funcname ) + 1;
  
    for( i = helper->nparents - 1 ; i > 0 ; i -- )
    {
      hstack[ i - 1 ] = hstack[ i ]->parent;
      len += strlen( hstack[ i ]->funcname ) + 1;
    }
	
    transport_write_u32( tpt, len );

    // replay helper key names      
    for( i = 0 ; i < helper->nparents ; i ++ )
    {
     transport_write_string( tpt, hstack[ i ]->funcname, strlen( hstack[ i ]->funcname ) );
     transport_write_string( tpt, ".", 1 ); 
    }
  }
  else // If helper has no parents, just use length of global
	  transport_write_u32( tpt, len );

  transport_write_string( tpt, helper->funcname, strlen( helper->funcname ) );
}

static void helper_wait_ready( Transport *tpt, u8 cmd )
{
  struct exception e;
  u8 cmdresp;

  transport_write_u8( tpt, cmd );
  cmdresp = transport_read_u8( tpt );
  if( cmdresp != RPC_READY )
  {
    e.errnum = ERR_PROTOCOL;
    e.type = nonfatal;
    Throw( e );
  }

}

static int helper_get( lua_State *L, Helper *helper )
{
  struct exception e;
  int freturn = 0;
  Transport *tpt = &helper->handle->tpt;
  
  Try
  {
    helper_wait_ready( tpt, RPC_CMD_GET );
    helper_remote_index( helper );
    
    read_variable( tpt, L );

    freturn = 1;
  }
  Catch( e )
  {
    freturn = generic_catch_handler( L, &helper->handle->tpt, e );
  }
  return freturn;
}




int helper_call (lua_State *L)
{
  struct exception e;
  int freturn = 0;
  Helper *h;
  Transport *tpt;
  
  h = ( Helper * )luaL_checkudata(L, 1, "rpc.helper");
  luaL_argcheck(L, h, 1, "helper expected");
  
  tpt = &h->handle->tpt;
  
  // capture special calls, otherwise execute normal remote call
  if( strcmp("get", h->funcname ) == 0 )
  {
    helper_get( L, h->parent );
    freturn = 1;
  }
  else
  {
    Try
    {
      int i,n;
      u32 nret,ret_code;

      // write function name
      tpt->timeout = tpt->com_timeout;     
      helper_wait_ready( tpt, RPC_CMD_CALL );
      helper_remote_index( h );

      // write number of arguments
      n = lua_gettop( L );
      transport_write_u32( tpt, n - 1 );
    
      // write each argument
      for( i = 2; i <= n; i ++ )
        write_variable( tpt, L, i );

      transport_flush(tpt);
      tpt->timeout = tpt->wait_timeout;

      /* if we're in async mode, we're done */
      /*if ( h->handle->async )
      {
        h->handle->read_reply_count++;
        freturn = 0;
      }*/

      // read return code
      ret_code = transport_read_u8( tpt );
      tpt->timeout = tpt->com_timeout;

      if ( ret_code == 0 )
      {
        // read return arguments
        nret = transport_read_u32( tpt );
      
        for ( i = 0; i < ( ( int ) nret ); i ++ )
          read_variable( tpt, L );
      
        freturn = ( int )nret;
      }
      else
      {
        // read error and handle it
        transport_read_u32( tpt ); // read code (not being used here)
        u32 len = transport_read_u32( tpt );
        char *err_string = ( char * )alloca( len + 1 );
        transport_read_string( tpt, err_string, len );
        err_string[ len ] = 0;

        deal_with_error( L, err_string );
        freturn = 0;
      }
    }
    Catch( e )
    {
      freturn = generic_catch_handler( L, &h->handle->tpt, e );
    }
  }
  return freturn;
}

// __newindex even on helper, 
int helper_newindex( lua_State *L )
{
  struct exception e;
  int freturn = 0;
  int ret_code;
  Helper *h;
  Transport *tpt;
  
  h = ( Helper * )luaL_checkudata(L, -3, "rpc.helper");
  luaL_argcheck(L, h, -3, "helper expected");
  
  luaL_checktype(L, -2, LUA_TSTRING );
  
  tpt = &h->handle->tpt;
  
  Try
  {  
    // index destination on remote side
    helper_wait_ready( tpt, RPC_CMD_NEWINDEX );
    helper_remote_index( h );

    write_variable( tpt, L, lua_gettop( L ) - 1 );
    write_variable( tpt, L, lua_gettop( L ) );

    ret_code = transport_read_u8( tpt );
    if( ret_code != 0 )
    {
      // read error and handle it
      transport_read_u32( tpt ); // Read code (not using here)
      u32 len = transport_read_u32( tpt );
      char *err_string = ( char * )alloca( len + 1 );
      transport_read_string( tpt, err_string, len );
      err_string[ len ] = 0;

      deal_with_error( L, err_string );
    }

    freturn = 0;
  }
  Catch( e )
  {
    freturn = generic_catch_handler( L, &h->handle->tpt, e );
  }
  return freturn;
}


static Helper *helper_append( lua_State *L, Helper *helper, const char *funcname )
{
  Helper *h = ( Helper * )lua_newuserdata( L, sizeof( Helper ) );
  luaL_getmetatable( L, "rpc.helper" );
  lua_setmetatable( L, -2 );
  
  lua_pushvalue( L, 1 ); // push parent
  h->pref = luaL_ref( L, LUA_REGISTRYINDEX ); // put ref into struct
  h->handle = helper->handle;
  h->parent = helper;
  h->nparents = helper->nparents + 1;
  strncpy ( h->funcname, funcname, NUM_FUNCNAME_CHARS );
  return h;
}

// indexing a helper returns a helper 
int helper_index( lua_State *L )
{
  const char *s;

  check_num_args( L, 2 );
  MYASSERT( lua_isuserdata( L, 1 ) && ismetatable_type( L, 1, "rpc.helper" ) );

  if( lua_type( L, 2 ) != LUA_TSTRING )
    return luaL_error( L, "can't index handle with non-string" );
  s = lua_tostring( L, 2 );
  if ( strlen( s ) > NUM_FUNCNAME_CHARS - 1 )
    return luaL_error( L, error_string( ERR_LONGFNAME ) );
  
  helper_append( L, ( Helper * )lua_touserdata( L, 1 ), s );

  return 1;
}

int helper_close (lua_State *L)
{
  Helper *h = ( Helper * )luaL_checkudata(L, 1, "rpc.helper");
  luaL_argcheck(L, h, 1, "helper expected");
  
  luaL_unref(L, LUA_REGISTRYINDEX, h->pref);
  h->pref = LUA_REFNIL;
  return 0;
}



//****************************************************************************
// lua remote function server
//   read function call data and execute the function. this function empties the
//   stack on entry and exit. This sets a custom error handler to catch errors 
//   around the function call.
#include <unistd.h>
static void read_cmd_call( Transport *tpt, lua_State *L )
{
  int i, stackpos, good_function, nargs;
  u32 len;
  char *funcname;
  char *token = NULL;
  // read function name

  len = transport_read_u32( tpt ); /* function name string length */ 
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;
    
  // get function
  // @@@ perhaps handle more like variables instead of using a long string?
  // @@@ also strtok is not thread safe
  token = strtok( funcname, "." );
  lua_getglobal( L, token );
  token = strtok( NULL, "." );
  while( token != NULL )
  {
    lua_getfield( L, -1, token );
    lua_remove( L, -2 );
    token = strtok( NULL, "." );
  }
  stackpos = lua_gettop( L ) - 1;
  good_function = LUA_ISCALLABLE( L, -1 );

  // read number of arguments
  nargs = transport_read_u32( tpt );

  // read in each argument, leave it on the stack
  for ( i = 0; i < nargs; i ++ ) 
    read_variable( tpt, L );

  // call the function
  if( good_function )
  {
    int nret, error_code;
    error_code = lua_pcall( L, nargs, LUA_MULTRET, 0 );
    
    // handle errors
    if ( error_code )
    {
      size_t len;
      const char *errmsg;
      errmsg = lua_tolstring (L, -1, &len);
      transport_write_u8( tpt, 1 );
      transport_write_u32( tpt, error_code );
      transport_write_u32( tpt, len );
      transport_write_string( tpt, errmsg, len );
    }
    else
    {
      // pass the return values back to the caller
      transport_write_u8( tpt, 0 );
      nret = lua_gettop( L ) - stackpos;
      transport_write_u32( tpt, nret );
      for ( i = 0; i < nret; i ++ )
        write_variable( tpt, L, stackpos + 1 + i );
    }
  }
  else
  {

    // bad function
    const char *msg = "undefined function: ";
    int errlen = strlen( msg ) + len;
    transport_write_u8( tpt, 1 );
    transport_write_u32( tpt, LUA_ERRRUN );
    transport_write_u32( tpt, errlen );
    transport_write_string( tpt, msg, strlen( msg ) );
    transport_write_string( tpt, funcname, len );
  }
  // empty the stack
  lua_settop ( L, 0 );
}


static void read_cmd_get( Transport *tpt, lua_State *L )
{
  u32 len;
  char *funcname;
  char *token = NULL;

  // read function name
  len = transport_read_u32( tpt ); // function name string length 
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;

  // get function
  // @@@ perhaps handle more like variables instead of using a long string?
  // @@@ also strtok is not thread safe
  token = strtok( funcname, "." );
  lua_getglobal( L, token );
  token = strtok( NULL, "." );
  while( token != NULL )
  {
    lua_getfield( L, -1, token );
    lua_remove( L, -2 );
    token = strtok( NULL, "." );
  }

  // return top value on stack
  write_variable( tpt, L, lua_gettop( L ) );

  // empty the stack
  lua_settop ( L, 0 );
}


static void read_cmd_newindex( Transport *tpt, lua_State *L )
{
  u32 len;
  char *funcname;
  char *token = NULL;

  // read function name
  len = transport_read_u32( tpt ); // function name string length
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;

  // get function
  // @@@ perhaps handle more like variables instead of using a long string?
  // @@@ also strtok is not thread safe
  if( strlen( funcname ) > 0 )
  {
    token = strtok( funcname, "." );
    lua_getglobal( L, token );
    token = strtok( NULL, "." );
    while( token != NULL )
    {
      lua_getfield( L, -1, token );
      lua_remove( L, -2 );
      token = strtok( NULL, "." );
    }
    read_variable( tpt, L ); // key
    read_variable( tpt, L ); // value
    lua_settable( L, -3 ); // set key to value on indexed table
  }
  else
  {
    read_variable( tpt, L ); // key
    read_variable( tpt, L ); // value
    lua_setglobal( L, lua_tostring( L, -2 ) );
  }
  // Write out 0 to indicate no error and that we're done
  transport_write_u8( tpt, 0 );
  
  // if ( error_code ) // Add some error handling later
  // {
  //   size_t len;
  //   const char *errmsg;
  //   errmsg = lua_tolstring (L, -1, &len);
  //   transport_write_u8( tpt, 1 );
  //   transport_write_u32( tpt, error_code );
  //   transport_write_u32( tpt, len );
  //   transport_write_string( tpt, errmsg, len );
  // }
  
  // empty the stack
  lua_settop ( L, 0 );
}

void rpc_dispatch_worker( lua_State *L, Transport* worker )
{  
  struct exception e;
  Try
    {

      switch ( transport_read_u8( worker ) )
        {
        case RPC_CMD_CALL:  // call function
          transport_write_u8( worker, RPC_READY );
          read_cmd_call( worker, L );
          break;
        case RPC_CMD_GET: // get server-side variable for client
          transport_write_u8( worker, RPC_READY );
          read_cmd_get( worker, L );
          break;
        case RPC_CMD_CON: //  allow client to renegotiate active connection
          server_negotiate( worker );
          break;
        case RPC_CMD_NEWINDEX: // assign new variable on server
          transport_write_u8( worker, RPC_READY );
          read_cmd_newindex( worker, L );
          break;
        default: // complain and throw exception if unknown command
          transport_write_u8(worker, RPC_UNSUPPORTED_CMD );
          e.type = nonfatal;
          e.errnum = ERR_COMMAND;
          Throw( e );
        }
      transport_flush(worker);
      //      handle->link_errs = 0;
    }
  Catch( e )
  {
    switch( e.type )
      {
      case fatal: // shutdown will initiate after throw
        //        server_handle_shutdown( handle );
        deal_with_error( L, error_string( e.errnum ) );
        break;
            
      case nonfatal:
        /*        handle->link_errs++;
        if ( handle->link_errs > MAX_LINK_ERRS )
          {
          handle->link_errs = 0;*/
        worker->must_die = 1;
        //        transport_remove_from_list(transport_list,worker);
        //        transport_delete(worker);

            //            transport_c
            //            Throw( e ); // remote connection will be closed
            //          }
        break;
            
      default: 
        Throw( e );
      }
    return;
  }

}


/*void rpc_dispatch_accept(Transport* listener)
{
  
  //  struct exception e;
    // if accepting transport is not open, accept a new connection from the
      // listening transport
  Transport* worker = &transports[transport_count];
  //  printf("luarpc: accepting\n");
  transport_count++;       
  transport_accept( listener, worker );
  //  printf("luarpc: accepting even more\n");
      switch ( transport_read_u8( worker ) )
      {
        case RPC_CMD_CON:
          //          printf("negitiating\n");
          server_negotiate( worker );
          break;
        default: // connection must be established to issue any other commands
          //          e.type = nonfatal;
          //e.errnum = ERR_COMMAND;
          transport_close( worker );
          //   Throw( e ); // remote connection will be closed
      }
      }*/


// **************************************************************************
// more error handling stuff 

// rpc_on_error( [ handle, ] error_handler )
static int rpc_on_error( lua_State *L )
{
  check_num_args( L, 1 );

  if( global_error_handler !=  LUA_NOREF )
    lua_unref (L,global_error_handler);
  
  global_error_handler = LUA_NOREF;

  if ( LUA_ISCALLABLE( L, 1 ) )
    global_error_handler = lua_ref( L, 1 );
  else if ( lua_isnil( L, 1 ) )
    { ;; }
  else
    return luaL_error( L, "bad args" );

  // @@@ add option for handle 
  // Handle *h = (Handle*) lua_touserdata (L,1); 
  // if (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.handle")); 

  return 0;
}


#endif
