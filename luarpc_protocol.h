#ifndef LUARPC_PROTOCOL_H
#define LUARPC_PROTOCOL_H

#include "luarpc_rpc.h" // struct Transport


void transport_write_uint8_t( Transport *tpt, uint8_t x );
uint8_t transport_read_uint8_t( Transport *tpt );
void transport_write_uint32_t( Transport *tpt, uint32_t x );
uint32_t transport_read_uint32_t( Transport *tpt );
void transport_read_string( Transport *tpt, char *buffer, int length );
void transport_write_string( Transport *tpt, const char *buffer, int length );

void server_negotiate( Transport *tpt );
void client_negotiate( Transport *tpt );
const char * error_string( int n );
void rpc_dispatch_worker( lua_State *L, Transport* worker );
//void rpc_dispatch_worker( lua_State *L, Transport* worker );
//void rpc_dispatch_accept(Transport* listener);
int ismetatable_type( lua_State *L, int ud, const char *tname );
extern int global_error_handler;
// Support for Compiling with & without rotables 
#ifdef LUA_OPTIMIZE_MEMORY
#define LUA_ISCALLABLE( state, idx ) ( lua_isfunction( state, idx ) || lua_islightfunction( state, idx ) )
#else
#define LUA_ISCALLABLE( state, idx ) lua_isfunction( state, idx )
#endif

int client_index (lua_State *L);
int client_newindex (lua_State *L);
int helper_newindex (lua_State *L);
int helper_call (lua_State *L);
int helper_index (lua_State *L);
int helper_close (lua_State *L);

#endif
