#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <bootstrap.h>

#include <lauxlib.h>
#include <lualib.h>

#define RIFT_SERVICE_NAME "com.acsandmann.rift"
#define MAX_MSG_SIZE 4096

typedef struct {
    mach_port_t port;
} rift_t;

static mach_port_t rift_connect_internal() {
    mach_port_t bootstrap_port;
    kern_return_t kr;

    kr = task_get_bootstrap_port(mach_task_self(), &bootstrap_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Failed to get bootstrap port: %s\n", mach_error_string(kr));
        return MACH_PORT_NULL;
    }

    mach_port_t server_port;
    kr = bootstrap_look_up(bootstrap_port, RIFT_SERVICE_NAME, &server_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Failed to look up Rift server port: %s\n", mach_error_string(kr));
        return MACH_PORT_NULL;
    }

    return server_port;
}

static char* rift_send_request_internal(mach_port_t server_port, const char* request_json, bool await_response) {
    if (server_port == MACH_PORT_NULL || request_json == NULL) {
        return NULL;
    }

    kern_return_t kr;

    mach_port_t reply_port = MACH_PORT_NULL;
    if (await_response) {
        kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "mach_port_allocate failed: %s\n", mach_error_string(kr));
            return NULL;
        }

        kr = mach_port_insert_right(mach_task_self(), reply_port, reply_port, MACH_MSG_TYPE_MAKE_SEND);
        if (kr != KERN_SUCCESS) {
            mach_port_deallocate(mach_task_self(), reply_port);
            fprintf(stderr, "mach_port_insert_right failed: %s\n", mach_error_string(kr));
            return NULL;
        }
    }

    size_t request_json_len = strlen(request_json) + 1;

    uint32_t aligned_len = (request_json_len + 3) & ~3;
    uint32_t total_size = sizeof(mach_msg_header_t) + aligned_len;

    if (total_size < 64) {
        total_size = 64;
    }

    char* request_buffer = calloc(1, total_size);
    if (!request_buffer) {
        if (await_response) mach_port_deallocate(mach_task_self(), reply_port);
        return NULL;
    }
    mach_msg_header_t* request_msg = (mach_msg_header_t*)request_buffer;

    request_msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, await_response ? MACH_MSG_TYPE_MAKE_SEND : 0);
    request_msg->msgh_local_port = reply_port;
    request_msg->msgh_remote_port = server_port;
    request_msg->msgh_size = total_size;
    request_msg->msgh_id = 1234;

    memcpy(request_buffer + sizeof(mach_msg_header_t), request_json, request_json_len);

    kr = mach_msg(request_msg, MACH_SEND_MSG, total_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

    free(request_buffer);

    if (kr != KERN_SUCCESS) {
        if (await_response) mach_port_deallocate(mach_task_self(), reply_port);
        fprintf(stderr, "mach_msg SEND failed: %s\n", mach_error_string(kr));
        return NULL;
    }

    if (!await_response) return (char*)1;

    char response_buffer[MAX_MSG_SIZE];
    mach_msg_header_t* response_msg = (mach_msg_header_t*)response_buffer;

    kr = mach_msg(response_msg, MACH_RCV_MSG, 0, MAX_MSG_SIZE, reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

    mach_port_deallocate(mach_task_self(), reply_port);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mach_msg RECEIVE failed: %s\n", mach_error_string(kr));
        return NULL;
    }

    char* response_json_ptr = (char*)response_msg + sizeof(mach_msg_header_t);
    size_t response_len = response_msg->msgh_size - sizeof(mach_msg_header_t);

    char* result = (char*)malloc(response_len + 1);
    if (result) {
        memcpy(result, response_json_ptr, response_len);
        result[response_len] = '\0';
    }

    return result;
}

static void rift_disconnect_internal(mach_port_t server_port) {}

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
        lua_pushstring(L, response_json);
        free(response_json);
    } else lua_pushboolean(L, 1);

    return 1;
}

static int l_rift_disconnect(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    rift_disconnect_internal(client->port);
    client->port = MACH_PORT_NULL; // Mark as disconnected
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
