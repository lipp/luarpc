// eLua platform configuration

#ifndef __PLATFORM_CONF_H__
#define __PLATFORM_CONF_H__

#ifdef LUA_RPC
#include "auxmods.h"

#define BUILD_RPC
#define LUARPC_ENABLE_SERIAL

#define LUA_PLATFORM_LIBS_ROM \
  _ROM( AUXLIB_RPC, luaopen_rpc, rpc_map )
#endif

#ifdef LUARPC_STANDALONE

#define BUILD_RPC
#define LUARPC_ENABLE_SERIAL
#endif

#endif // #ifndef __PLATFORM_CONF_H__
