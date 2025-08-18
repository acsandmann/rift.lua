#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lauxlib.h>
#include <lualib.h>

#include "mach.h"
#include "parsing.h"

static int l_rift_connect(lua_State *L) {
    mach_port_t port = rift_connect_internal();
    if (port == MACH_PORT_NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to connect to Rift server.");
        return 2;
    }

    rift_t *client = (rift_t*)lua_newuserdata(L, sizeof(rift_t));
    client->port = port;

    luaL_newmetatable(L, "rift.client");
    lua_setmetatable(L, -2);

    return 1;
}

static int l_rift_send_request(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    const char *request_json = luaL_checkstring(L, 2);
    bool await_response = true;
    if (lua_gettop(L) >= 3) {
        await_response = lua_toboolean(L, 3);
    }

    char* response_json = rift_send_request_internal(client->port, request_json, await_response);

    if (response_json == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Request failed in C module.");
        return 2;
    }

    if (await_response) {
        bool res = json_to_lua_table(L, response_json);
        free(response_json);
        if (!res) {
            lua_pushnil(L);
            lua_pushstring(L, "Failed to parse JSON response.");
            return 2;
        }
    } else lua_pushboolean(L, 1);

    return 1;
}

static int l_rift_disconnect(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    rift_disconnect_internal(client->port);
    client->port = MACH_PORT_NULL;
    return 0;
}

static int l_rift_gc(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    if (client->port != MACH_PORT_NULL) rift_disconnect_internal(client->port);
    return 0;
}


static const struct luaL_Reg rift_lib[] = {
    {"connect", l_rift_connect},
    {"send_request", l_rift_send_request},
    {"disconnect", l_rift_disconnect},
    {NULL, NULL}
};

int luaopen_rift(lua_State *L) {
    luaL_newlib(L, rift_lib);

    if (luaL_newmetatable(L, "rift.client")) {
        lua_pushstring(L, "__gc");
        lua_pushcfunction(L, l_rift_gc);
        lua_settable(L, -3);
    }
    lua_pop(L, 1);

    return 1;
}
