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
#ifdef __MINGW32__
void *alloca(size_t);
#elif WIN32
#define alloca _alloca
else
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


struct timeval timeval_from_ms( double ms ){
  struct timeval t;
  t.tv_sec = (int)(ms / 1000);
  ms -= t.tv_sec * 1000;
  t.tv_usec = (int)(ms * 1000);
  return t;
}

double ms_from_timeval( struct timeval t ){
  return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

Transport * transport_create (void){
  Transport* t = malloc(sizeof(Transport));
  memset(t,0,sizeof(Transport));
  t->fd = -1;
  t->wait_timeout.tv_sec = 3;
  t->com_timeout.tv_sec = 1;
  return t;
}

struct transport_node* transport_list;


#ifdef WIN32
static void net_startup()
{
  struct exception e;
  WORD wVersionRequested;
  WSADATA wsaData;
  int err;

  // startup WinSock version 2
  wVersionRequested = MAKEWORD(2,0);
  err = WSAStartup (wVersionRequested, &wsaData);
  if (err != 0) {
    e.errnum = WSAGetLastError();
    e.type = fatal;
    Throw( e );
  }

  // confirm that the WinSock DLL supports 2.0. note that if the DLL
  // supports versions greater than 2.0 in addition to 2.0, it will
  // still return 2.0 in wVersion since that is the version we requested.
  if (LOBYTE (wsaData.wVersion ) != 2 ||
    HIBYTE(wsaData.wVersion) != 0 ) {
      WSACleanup();
      if (err != 0) {
        e.errnum = WSAGetLastError();
        e.type = fatal;
        Throw( e );
      }
  }
}
#endif


struct transport_node* transport_new_list(){
  struct transport_node* node = (struct transport_node*) malloc(sizeof(struct transport_node));  
  //  printf("node %p\n",node);
  node->t = NULL;
  node->prev = node;
  node->next = node;
  return node;
}

void transport_insert_to_list(struct transport_node* head, Transport* t){
  struct transport_node* node = (struct transport_node*) malloc(sizeof(struct transport_node));
  memset(node,0,sizeof(struct transport_node));
  //  printf("node %p\n",node);
  node->t = t;
  node->prev = head;
  node->next = head->next;
  head->next = node;
  node->next->prev = node;
}

struct transport_node* transport_remove_from_list(struct transport_node* head, Transport* t){
  struct transport_node* node = head;
  struct transport_node* prev = head;
  while( (node = node->next) != head ){
    if( node->t == t ){
      node->next->prev = node->prev;
      node->prev->next = node->next;
      prev = node->prev;
      free(node);
      break;
    }
  }
  return prev;
}


struct exception_context the_exception_context[ 1 ];
//static Helper *helper_create( lua_State *L, Handle *handle, const char *funcname );
Handle *handle_create( lua_State *L );

// **************************************************************************
// server side handle userdata objects. 

static ServerHandle *server_handle_create( lua_State *L )
{
  ServerHandle *h = ( ServerHandle * )lua_newuserdata( L, sizeof( ServerHandle ) );
  luaL_getmetatable( L, "rpc.server_handle" );
  lua_setmetatable( L, -2 );

  h->link_errs = 0;

  transport_init( &h->ltpt );
  //  transport_init( &h->atpt );
  return h;
}

static void server_handle_shutdown( ServerHandle *h )
{
  printf("server shutdown\n");
  //  transport_close( &h->ltpt );
  
  //  transport_close( &h->atpt );
}

static void server_handle_destroy( ServerHandle *h )
{
  server_handle_shutdown( h );
}

// **************************************************************************
// remote function calling (client side)

// rpc_connect (ip_address, port)
//      returns a handle to the new connection, or nil if there was an error.
//      if there is an RPC error function defined, it will be called on error.


static int rpc_connect( lua_State *L )
{
  struct exception e;
  Handle *handle = 0;
  
  Try
  {
    double com_timeout_ms = luaL_optnumber(L,3,1000.0);
    double wait_timeout_ms = luaL_optnumber(L,5,3000.0);
    handle = handle_create ( L );

    handle->tpt.com_timeout = timeval_from_ms( com_timeout_ms );
    handle->tpt.wait_timeout = timeval_from_ms( wait_timeout_ms );
    

    handle->tpt.timeout = handle->tpt.com_timeout;
    transport_open_connection( L, handle );    
    client_negotiate( &handle->tpt );
  }
  Catch( e )
  {     
    deal_with_error( L, error_string( e.errnum ) );
    lua_pushnil( L );
  }
  return 1;
}


// rpc_close( handle )
//     this closes the transport, but does not free the handle object. that's
//     because the handle will still be in the user's name space and might be
//     referred to again. we'll let garbage collection free the object.
//     it's a lua runtime error to refer to a transport after it has been closed.


static int rpc_close( lua_State *L )
{
  check_num_args( L, 1 );

  if( lua_isuserdata( L, 1 ) )
  {
    if( ismetatable_type( L, 1, "rpc.handle" ) )
    {
      Handle *handle = ( Handle * )lua_touserdata( L, 1 );
      //      transport_delete( &handle->tpt );

#ifdef WIN32
      closesocket(handle->tpt.fd);
#else
      fclose(handle->tpt.file);
#endif
      return 0;
    }
    if( ismetatable_type( L, 1, "rpc.server_handle" ) )
    {
      ServerHandle *handle = ( ServerHandle * )lua_touserdata( L, 1 );
      server_handle_shutdown( handle );
      return 0;
    }
  }

  return luaL_error(L,"arg must be handle");
}

static int rpc_wait_timeout( lua_State *L )
{
  //  check_num_args( L, 2 );

  if( lua_isuserdata( L, 1 ) )
  {
    if( ismetatable_type( L, 1, "rpc.handle" ) )
    {
      Handle *handle = ( Handle * )lua_touserdata( L, 1 );
      double timeout_ms = luaL_optnumber(L,2,-1.0);
      if( timeout_ms != -1.0 ){
        handle->tpt.wait_timeout = timeval_from_ms( timeout_ms );
        return 0;
      }
      else {
        lua_pushnumber(L,ms_from_timeval( handle->tpt.wait_timeout ) );
        return 1;
      }
    }
  }
  return luaL_error(L,"arg must be rpc.handle");
}

static int rpc_com_timeout( lua_State *L )
{
  //  check_num_args( L, 1 );

  if( lua_isuserdata( L, 1 ) )
  {
    if( ismetatable_type( L, 1, "rpc.handle" ) )
    {
      Handle *handle = ( Handle * )lua_touserdata( L, 1 );
      
      double timeout_ms = luaL_optnumber(L,2,-1.0);
      if( timeout_ms != -1.0 ){
        handle->tpt.com_timeout = timeval_from_ms( timeout_ms );
        return 0;
      }
      else {
        lua_pushnumber(L,ms_from_timeval( handle->tpt.com_timeout ) );
        return 1;
      }
    }

    if( ismetatable_type( L, 1, "rpc.server_handle" ) )
    {
      ServerHandle *handle = ( ServerHandle * )lua_touserdata( L, 1 );

      double timeout_ms = luaL_optnumber(L,2,-1.0);
      if( timeout_ms != -1.0 ){
        handle->ltpt.com_timeout = timeval_from_ms( timeout_ms );
        return 0;
      }
      else {
        lua_pushnumber(L,ms_from_timeval( handle->ltpt.com_timeout ) );
        return 1;
      }
    }
  }

  return luaL_error(L,"arg must be handle");
}


// rpc_async (handle,)
//     this sets a handle's asynchronous calling mode (0/nil=off, other=on).
//     (this is for the client only).
//     @@@ Before re-enabling, this should be brought up to date with multi-command architecture

// static int rpc_async (lua_State *L)
// {
//   Handle *handle;
//   check_num_args( L, 2 );
// 
//   if ( !lua_isuserdata( L, 1 ) || !ismetatable_type( L, 1, "rpc.handle" ) )
//     my_lua_error( L, "first arg must be client handle" );
// 
//   handle = ( Handle * )lua_touserdata( L, 1 );
// 
//   if ( lua_isnil( L, 2 ) || ( lua_isnumber( L, 2 ) && lua_tonumber( L, 2 ) == 0) )
//     handle->async = 0;
//   else
//     handle->async = 1;
// 
//   return 0;
// }


static ServerHandle *rpc_listen_helper( lua_State *L )
{
  struct exception e;
  ServerHandle *handle = 0;

  Try
  {
    // make server handle 
    handle = server_handle_create( L );

    // make listening transport 
    transport_open_listener( L, handle );
  }
  Catch( e )
  {
    if( handle )
      server_handle_destroy( handle );
    
    deal_with_error( L, error_string( e.errnum ) );
    return 0;
  }
  return handle;
}


static void rpc_dispatch_accept(Transport* listener)
{
  
  struct exception e;
    // if accepting transport is not open, accept a new connection from the
      // listening transport
  Transport* worker = transport_create();
  worker->timeout = worker->com_timeout;
  transport_insert_to_list(transport_list,worker);
  Try{
    transport_accept( listener, worker );
    server_negotiate( worker );
  }
  Catch(e){
    transport_remove_from_list(transport_list,worker);
    transport_delete( worker );
  }
}


// rpc_server( transport_identifier )
static int rpc_server( lua_State *L )
{
  int shref;
  ServerHandle *handle;

  struct transport_node* node;
  Transport* listener;

  handle = rpc_listen_helper( L );
  listener = &handle->ltpt; 

  transport_list = transport_new_list();
  transport_insert_to_list( transport_list, listener ); 
  node = transport_list;
  // Anchor handle in the registry
  //   This is needed because garbage collection can steal our handle, 
  //   which isn't otherwise referenced
  //
  //   @@@ this should be replaced when we create a system for multiple 
  //   @@@ connections. such a mechanism would likely likely create a
  //   @@@ table for multiple connections that we could service in an event loop 
  
  shref = luaL_ref( L, LUA_REGISTRYINDEX );
  lua_rawgeti(L, LUA_REGISTRYINDEX, shref );
  
  while ( transport_is_open( listener ) ){
    //    printf("luarpc: listening on %p\n",(void*)listener);
    if( transport_select(transport_list) > -1 ){
      while( (node = node->next) != transport_list ){
        Transport* worker = node->t;
        if( worker->is_set && worker != listener ){
          rpc_dispatch_worker(L,worker);
        }
      }
      while( (node = node->next) != transport_list ){
        Transport* worker = node->t;        
        if( worker->must_die ){
          node = transport_remove_from_list(transport_list,worker);
          transport_delete(worker);
        }
      }
      if( listener->is_set ){
        rpc_dispatch_accept(listener);     
      }
    }
  }
    
  luaL_unref( L, LUA_REGISTRYINDEX, shref );
  server_handle_destroy( handle );
  return 0;
}

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

// **************************************************************************
// register RPC functions 

/*
#ifndef LUARPC_STANDALONE

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"

const LUA_REG_TYPE rpc_handle[] =
{
  { LSTRKEY( "__index" ), LFUNCVAL( handle_index ) },
  { LSTRKEY( "__newindex"), LFUNCVAL( handle_newindex )},
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_helper[] =
{
  { LSTRKEY( "__call" ), LFUNCVAL( helper_call ) },
  { LSTRKEY( "__index" ), LFUNCVAL( helper_index ) },
  { LSTRKEY( "__newindex" ), LFUNCVAL( helper_newindex ) },
  { LSTRKEY( "__gc" ), LFUNCVAL( helper_close ) },
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_server_handle[] =
{
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_map[] =
{
  {  LSTRKEY( "connect" ), LFUNCVAL( rpc_connect ) },
  {  LSTRKEY( "close" ), LFUNCVAL( rpc_close ) },
  {  LSTRKEY( "server" ), LFUNCVAL( rpc_server ) },
  {  LSTRKEY( "on_error" ), LFUNCVAL( rpc_on_error ) },
  {  LSTRKEY( "listen" ), LFUNCVAL( rpc_listen ) },
  {  LSTRKEY( "peek" ), LFUNCVAL( rpc_peek ) },
  {  LSTRKEY( "dispatch" ), LFUNCVAL( rpc_dispatch ) },
//  {  LSTRKEY( "rpc_async" ), LFUNCVAL( rpc_async ) },
#if LUA_OPTIMIZE_MEMORY > 0
// {  LSTRKEY("mode"), LSTRVAL( LUARPC_MODE ) }, 
#endif // #if LUA_OPTIMIZE_MEMORY > 0
  { LNILKEY, LNILVAL }
};


LUALIB_API int luaopen_rpc(lua_State *L)
{
#if LUA_OPTIMIZE_MEMORY > 0
  luaL_rometatable(L, "rpc.helper", (void*)rpc_helper);
  luaL_rometatable(L, "rpc.handle", (void*)rpc_handle);
  luaL_rometatable(L, "rpc.server_handle", (void*)rpc_server_handle);
#else
  luaL_register( L, "rpc", rpc_map );
  lua_pushstring( L, LUARPC_MODE );
  lua_setfield(L, -2, "mode");

  luaL_newmetatable( L, "rpc.helper" );
  luaL_register( L, NULL, rpc_helper );
  
  luaL_newmetatable( L, "rpc.handle" );
  luaL_register( L, NULL, rpc_handle );
  
  luaL_newmetatable( L, "rpc.server_handle" );
#endif
  return 1;
}

#else

static const luaL_reg rpc_handle[] =
{
  { "__index", handle_index },
  { "__newindex", handle_newindex },
  { NULL, NULL }
};

static const luaL_reg rpc_helper[] =
{
  { "__call", helper_call },
  { "__index", helper_index },
  { "__newindex", helper_newindex },
  { "__gc", helper_close },
  { NULL, NULL }
};

static const luaL_reg rpc_server_handle[] =
{
  { NULL, NULL }
};

static const luaL_reg rpc_map[] =
{
  { "connect", rpc_connect },
  { "close", rpc_close },
  { "server", rpc_server },
  { "on_error", rpc_on_error },
  //  { "listen", rpc_listen },
  //  { "peek", rpc_peek },
  //  { "dispatch", rpc_dispatch },
//  { "rpc_async", rpc_async },
  { NULL, NULL }
};


LUALIB_API int luaopen_rpc(lua_State *L)
{
  luaL_register( L, "rpc", rpc_map );
  lua_pushstring(L, LUARPC_MODE);
  lua_setfield(L, -2, "mode");

  luaL_newmetatable( L, "rpc.helper" );
  luaL_register( L, NULL, rpc_helper );
  
  luaL_newmetatable( L, "rpc.handle" );
  luaL_register( L, NULL, rpc_handle );
  
  luaL_newmetatable( L, "rpc.server_handle" );

  return 1;
}

#endif

#endif
*/

// **************************************************************************
// register RPC functions 

#ifndef LUARPC_STANDALONE

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"

const LUA_REG_TYPE rpc_handle[] =
{
  { LSTRKEY( "__index" ), LFUNCVAL( handle_index ) },
  { LSTRKEY( "__newindex"), LFUNCVAL( handle_newindex )},
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_helper[] =
{
  { LSTRKEY( "__call" ), LFUNCVAL( helper_call ) },
  { LSTRKEY( "__index" ), LFUNCVAL( helper_index ) },
  { LSTRKEY( "__newindex" ), LFUNCVAL( helper_newindex ) },
  { LSTRKEY( "__gc" ), LFUNCVAL( helper_close ) },
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_server_handle[] =
{
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_map[] =
{
  {  LSTRKEY( "connect" ), LFUNCVAL( rpc_connect ) },
  {  LSTRKEY( "close" ), LFUNCVAL( rpc_close ) },
  {  LSTRKEY( "server" ), LFUNCVAL( rpc_server ) },
  {  LSTRKEY( "on_error" ), LFUNCVAL( rpc_on_error ) },
  //  {  LSTRKEY( "listen" ), LFUNCVAL( rpc_listen ) },
  //  {  LSTRKEY( "peek" ), LFUNCVAL( rpc_peek ) },
  //  {  LSTRKEY( "dispatch" ), LFUNCVAL( rpc_dispatch ) },
//  {  LSTRKEY( "rpc_async" ), LFUNCVAL( rpc_async ) },
#if LUA_OPTIMIZE_MEMORY > 0
// {  LSTRKEY("mode"), LSTRVAL( LUARPC_MODE ) }, 
#endif // #if LUA_OPTIMIZE_MEMORY > 0
  { LNILKEY, LNILVAL }
};


LUALIB_API int luaopen_rpc(lua_State *L)
{
#if LUA_OPTIMIZE_MEMORY > 0
  luaL_rometatable(L, "rpc.helper", (void*)rpc_helper);
  luaL_rometatable(L, "rpc.handle", (void*)rpc_handle);
  luaL_rometatable(L, "rpc.server_handle", (void*)rpc_server_handle);
#else
  luaL_register( L, "rpc", rpc_map );
  lua_pushstring( L, LUARPC_MODE );
  lua_setfield(L, -2, "mode");

  luaL_newmetatable( L, "rpc.helper" );
  luaL_register( L, NULL, rpc_helper );
  
  luaL_newmetatable( L, "rpc.handle" );
  luaL_register( L, NULL, rpc_handle );
  
  luaL_newmetatable( L, "rpc.server_handle" );
#endif
  return 1;
}

#else


static const luaL_reg rpc_handle[] =
{
  { "__index", handle_index },
  { "__newindex", handle_newindex },
  { NULL, NULL }
};

static const luaL_reg rpc_helper[] =
{
  { "__call", helper_call },
  { "__index", helper_index },
  { "__newindex", helper_newindex },
  { "__gc", helper_close },
  { NULL, NULL }
};

static const luaL_reg rpc_server_handle[] =
{
  { NULL, NULL }
};

static const luaL_reg rpc_map[] =
{
  { "connect", rpc_connect },
  { "close", rpc_close },
  { "server", rpc_server },
  { "on_error", rpc_on_error },
  { "com_timeout", rpc_com_timeout },
  { "wait_timeout", rpc_wait_timeout },
  { NULL, NULL }
};


LUALIB_API int luaopen_rpc(lua_State *L)
{
  luaL_register( L, "rpc", rpc_map );
  lua_pushstring(L, LUARPC_MODE);
  lua_setfield(L, -2, "mode");

  luaL_newmetatable( L, "rpc.helper" );
  luaL_register( L, NULL, rpc_helper );
  
  luaL_newmetatable( L, "rpc.handle" );
  luaL_register( L, NULL, rpc_handle );
  
  luaL_newmetatable( L, "rpc.server_handle" );

#ifdef WIN32
  net_startup();
#endif
  return 1;
}

#endif

