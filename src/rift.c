#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <lauxlib.h>
#include <lualib.h>

#include "mach.h"
#include "parsing.h"

#define RIFT_CB_STORE_KEY "rift.client.callback_store"
#define RIFT_TIMER_STORE_KEY "rift.client.timer_store"
#define RIFT_CLIENT_KEEPALIVE_KEY "rift.client.keepalive"
#define RIFT_AUTO_PUMP_INTERVAL_SECONDS 0.01

static int rift_send_event_subscription_request(lua_State *L, rift_t *client, const char *key, const char *event);
static char* rift_extract_event_type(const char *event_json);

typedef struct {
    lua_State *L;
    rift_t *client;
    CFRunLoopTimerRef timer;
} rift_timer_ctx_t;

static void rift_push_callback_store(lua_State *L, bool create) {
    lua_pushstring(L, RIFT_CB_STORE_KEY);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        if (!create) {
            lua_pushnil(L);
            return;
        }

        lua_newtable(L);
        lua_pushstring(L, RIFT_CB_STORE_KEY);
        lua_pushvalue(L, -2);
        lua_settable(L, LUA_REGISTRYINDEX);
    }
}

static bool rift_push_client_callback_list(lua_State *L, rift_t *client, bool create) {
    rift_push_callback_store(L, create);
    if (!lua_istable(L, -1)) {
        return false;
    }

    lua_pushlightuserdata(L, (void*)client);
    lua_gettable(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        if (!create) {
            lua_pop(L, 1);
            lua_pushnil(L);
            return false;
        }

        lua_newtable(L);
        lua_pushlightuserdata(L, (void*)client);
        lua_pushvalue(L, -2);
        lua_settable(L, -4);
    }

    lua_remove(L, -2);
    return true;
}

static void rift_push_keepalive_store(lua_State *L, bool create) {
    lua_pushstring(L, RIFT_CLIENT_KEEPALIVE_KEY);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        if (!create) {
            lua_pushnil(L);
            return;
        }
        lua_newtable(L);
        lua_pushstring(L, RIFT_CLIENT_KEEPALIVE_KEY);
        lua_pushvalue(L, -2);
        lua_settable(L, LUA_REGISTRYINDEX);
    }
}

static void rift_retain_client(lua_State *L, rift_t *client, int client_index) {
    rift_push_keepalive_store(L, true);
    lua_pushlightuserdata(L, (void*)client);
    lua_pushvalue(L, client_index);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

static void rift_release_client(lua_State *L, rift_t *client) {
    rift_push_keepalive_store(L, false);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushlightuserdata(L, (void*)client);
    lua_pushnil(L);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

static void rift_push_timer_store(lua_State *L, bool create) {
    lua_pushstring(L, RIFT_TIMER_STORE_KEY);
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        if (!create) {
            lua_pushnil(L);
            return;
        }

        lua_newtable(L);
        lua_pushstring(L, RIFT_TIMER_STORE_KEY);
        lua_pushvalue(L, -2);
        lua_settable(L, LUA_REGISTRYINDEX);
    }
}

static rift_timer_ctx_t* rift_get_timer_ctx(lua_State *L, rift_t *client) {
    rift_push_timer_store(L, false);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }

    lua_pushlightuserdata(L, (void*)client);
    lua_gettable(L, -2);
    rift_timer_ctx_t *ctx = NULL;
    if (lua_islightuserdata(L, -1)) {
        ctx = (rift_timer_ctx_t*)lua_touserdata(L, -1);
    }
    lua_pop(L, 2);
    return ctx;
}

static void rift_set_timer_ctx(lua_State *L, rift_t *client, rift_timer_ctx_t *ctx) {
    rift_push_timer_store(L, true);
    lua_pushlightuserdata(L, (void*)client);
    if (ctx) lua_pushlightuserdata(L, (void*)ctx);
    else lua_pushnil(L);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

static void rift_clear_client_callback_list(lua_State *L, rift_t *client) {
    rift_push_callback_store(L, false);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    lua_pushlightuserdata(L, (void*)client);
    lua_pushnil(L);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

static int rift_pump_once_internal(lua_State *L, rift_t *client, mach_msg_timeout_t timeout_ms, bool push_lua_error) {
    if (client->event_port == MACH_PORT_NULL) {
        return 0;
    }

    bool timed_out = false;
    char *event_json = rift_try_receive_event_internal(client->event_port, timeout_ms, &timed_out);
    if (!event_json) {
        if (timed_out) {
            return 0;
        }
        if (push_lua_error) {
            lua_pushnil(L);
            lua_pushstring(L, "Failed to receive event.");
        } else {
            fprintf(stderr, "rift auto-pump: failed to receive event.\n");
        }
        return -1;
    }

    char *event_type = rift_extract_event_type(event_json);

    if (!rift_push_client_callback_list(L, client, false)) {
        free(event_json);
        if (event_type) free(event_type);
        return 0;
    }

    int dispatched = 0;
    lua_Integer cb_count = (lua_Integer)lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= cb_count; ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        bool should_dispatch = false;
        lua_getfield(L, -1, "events");
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "*");
            lua_gettable(L, -2);
            should_dispatch = lua_toboolean(L, -1);
            lua_pop(L, 1);

            if (!should_dispatch && event_type) {
                lua_pushstring(L, event_type);
                lua_gettable(L, -2);
                should_dispatch = lua_toboolean(L, -1);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        if (!should_dispatch) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "callback");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            continue;
        }

        lua_newtable(L);
        lua_pushstring(L, event_json);
        lua_setfield(L, -2, "INFO");
        if (event_type) {
            lua_pushstring(L, event_type);
            lua_setfield(L, -2, "EVENT");
        } else {
            lua_pushnil(L);
            lua_setfield(L, -2, "EVENT");
        }

        if (json_to_lua_table(L, event_json)) {
            lua_setfield(L, -2, "DATA");
        } else {
            lua_pushnil(L);
            lua_setfield(L, -2, "DATA");
        }

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *cb_err = lua_tostring(L, -1);
            lua_pop(L, 2);
            free(event_json);
            if (event_type) free(event_type);
            if (push_lua_error) {
                lua_pushnil(L);
                lua_pushfstring(L, "Pump callback failed: %s", cb_err ? cb_err : "unknown error");
            } else {
                fprintf(stderr, "rift auto-pump callback error: %s\n", cb_err ? cb_err : "unknown error");
            }
            return -1;
        }

        dispatched++;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    free(event_json);
    if (event_type) free(event_type);

    return dispatched;
}

static void rift_timer_callback(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    rift_timer_ctx_t *ctx = (rift_timer_ctx_t*)info;
    if (!ctx || !ctx->L || !ctx->client) return;

    lua_State *L = ctx->L;
    int top = lua_gettop(L);
    while (1) {
        int rc = rift_pump_once_internal(L, ctx->client, 0, false);
        if (rc <= 0) break;
    }
    lua_settop(L, top);
}

static bool rift_start_auto_pump(lua_State *L, rift_t *client) {
    rift_timer_ctx_t *existing = rift_get_timer_ctx(L, client);
    if (existing && existing->timer) return true;

    rift_timer_ctx_t *ctx = (rift_timer_ctx_t*)calloc(1, sizeof(rift_timer_ctx_t));
    if (!ctx) return false;
    ctx->L = L;
    ctx->client = client;

    CFRunLoopTimerContext timer_ctx = {
        0,
        ctx,
        NULL,
        NULL,
        NULL
    };

    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    ctx->timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        now,
        RIFT_AUTO_PUMP_INTERVAL_SECONDS,
        0,
        0,
        rift_timer_callback,
        &timer_ctx
    );
    if (!ctx->timer) {
        free(ctx);
        return false;
    }

    CFRunLoopAddTimer(CFRunLoopGetMain(), ctx->timer, kCFRunLoopCommonModes);
    rift_set_timer_ctx(L, client, ctx);
    return true;
}

static void rift_stop_auto_pump(lua_State *L, rift_t *client) {
    rift_timer_ctx_t *ctx = rift_get_timer_ctx(L, client);
    if (!ctx) return;

    if (ctx->timer) {
        CFRunLoopTimerInvalidate(ctx->timer);
        CFRelease(ctx->timer);
        ctx->timer = NULL;
    }
    rift_set_timer_ctx(L, client, NULL);
    free(ctx);
}

static int rift_subscribe_events(lua_State *L, rift_t *client, int table_index) {
    uint32_t event_count = (uint32_t)lua_rawlen(L, table_index);
    if (event_count == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Events table cannot be empty.");
        return 2;
    }

    for (uint32_t i = 0; i < event_count; ++i) {
        lua_rawgeti(L, table_index, (lua_Integer)(i + 1));
        const char *event = luaL_checkstring(L, -1);
        int rc = rift_send_event_subscription_request(L, client, "subscribe", event);
        lua_pop(L, 1);
        if (rc != 1) {
            return rc;
        }
        lua_pop(L, 1);
    }

    return 1;
}

static int rift_resubscribe_callback_events(lua_State *L, rift_t *client) {
    if (!rift_push_client_callback_list(L, client, false)) {
        return 1;
    }

    lua_Integer cb_count = (lua_Integer)lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= cb_count; ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "events");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 2);
            continue;
        }

        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char *event = lua_tostring(L, -2);
            if (event) {
                int rc = rift_send_event_subscription_request(L, client, "subscribe", event);
                lua_pop(L, 1);
                if (rc != 1) {
                    lua_pop(L, 2);
                    lua_pop(L, 1);
                    return rc;
                }
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
            }
        }

        lua_pop(L, 2);
    }

    lua_pop(L, 1);
    return 1;
}

static char* rift_extract_event_type(const char *event_json) {
    if (!event_json) return NULL;
    cJSON *root = cJSON_Parse(event_json);
    if (!root) return NULL;

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    char *out = NULL;
    if (cJSON_IsString(type) && type->valuestring) {
        size_t len = strlen(type->valuestring);
        out = (char*)malloc(len + 1);
        if (out) memcpy(out, type->valuestring, len + 1);
    }

    cJSON_Delete(root);
    return out;
}

static bool rift_ensure_event_port(lua_State *L, rift_t *client) {
    if (client->server_port == MACH_PORT_NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Client is disconnected.");
        return false;
    }

    if (client->event_port == MACH_PORT_NULL) {
        client->event_port = rift_allocate_reply_port_internal();
        if (client->event_port == MACH_PORT_NULL) {
            lua_pushnil(L);
            lua_pushstring(L, "Failed to allocate event stream port.");
            return false;
        }
    }

    return true;
}

static int l_rift_reconnect(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");

    if (client->event_port != MACH_PORT_NULL) {
        rift_deallocate_reply_port_internal(client->event_port);
        client->event_port = MACH_PORT_NULL;
    }
    if (client->server_port != MACH_PORT_NULL) {
        rift_disconnect_internal(client->server_port);
        client->server_port = MACH_PORT_NULL;
    }

    client->server_port = rift_connect_internal();
    if (client->server_port == MACH_PORT_NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to reconnect to Rift server.");
        return 2;
    }

    client->event_port = rift_allocate_reply_port_internal();
    if (client->event_port == MACH_PORT_NULL) {
        rift_disconnect_internal(client->server_port);
        client->server_port = MACH_PORT_NULL;
        lua_pushnil(L);
        lua_pushstring(L, "Failed to allocate event stream port on reconnect.");
        return 2;
    }

    int rc = rift_resubscribe_callback_events(L, client);
    if (rc != 1) {
        return rc;
    }

    lua_settop(L, 1);
    return 1;
}

static int rift_send_event_subscription_request(lua_State *L, rift_t *client, const char *key, const char *event) {
    cJSON *root = cJSON_CreateObject();
    cJSON *sub = cJSON_CreateObject();
    if (!root || !sub) {
        if (root) cJSON_Delete(root);
        if (sub) cJSON_Delete(sub);
        lua_pushnil(L);
        lua_pushstring(L, "Failed to build subscription request.");
        return 2;
    }

    cJSON_AddStringToObject(sub, "event", event);
    cJSON_AddItemToObject(root, key, sub);

    char *request_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!request_json) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to serialize subscription request.");
        return 2;
    }

    char *response_json = rift_send_request_with_reply_port_internal(
        client->server_port,
        client->event_port,
        request_json,
        true
    );
    cJSON_free(request_json);

    if (!response_json) {
        lua_pushnil(L);
        lua_pushstring(L, "Subscription request failed in C module.");
        return 2;
    }

    bool ok = json_to_lua_table(L, response_json);
    free(response_json);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to parse subscription response JSON.");
        return 2;
    }

    return 1;
}

static int l_rift_connect(lua_State *L) {
    mach_port_t port = rift_connect_internal();
    if (port == MACH_PORT_NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to connect to Rift server.");
        return 2;
    }

    rift_t *client = (rift_t*)lua_newuserdata(L, sizeof(rift_t));
    client->server_port = port;
    client->event_port = MACH_PORT_NULL;

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

    char* response_json = rift_send_request_internal(client->server_port, request_json, await_response);

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
    rift_release_client(L, client);
    if (client->event_port != MACH_PORT_NULL) {
        rift_deallocate_reply_port_internal(client->event_port);
        client->event_port = MACH_PORT_NULL;
    }
    rift_disconnect_internal(client->server_port);
    client->server_port = MACH_PORT_NULL;
    return 0;
}

static int l_rift_gc(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    rift_release_client(L, client);
    rift_stop_auto_pump(L, client);
    rift_clear_client_callback_list(L, client);
    if (client->event_port != MACH_PORT_NULL) {
        rift_deallocate_reply_port_internal(client->event_port);
        client->event_port = MACH_PORT_NULL;
    }
    if (client->server_port != MACH_PORT_NULL) rift_disconnect_internal(client->server_port);
    return 0;
}

static int l_rift_subscribe(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    rift_retain_client(L, client, 1);

    if (!rift_ensure_event_port(L, client)) return 2;

    if (lua_type(L, 2) == LUA_TSTRING) {
        const char *event = lua_tostring(L, 2);
        return rift_send_event_subscription_request(L, client, "subscribe", event);
    }

    luaL_checktype(L, 2, LUA_TTABLE);
    bool has_callback = (lua_gettop(L) >= 3 && lua_type(L, 3) == LUA_TFUNCTION);

    int rc = rift_subscribe_events(L, client, 2);
    if (rc != 1) return rc;

    if (!has_callback) {
        lua_pushboolean(L, 1);
        return 1;
    }

    if (!rift_push_client_callback_list(L, client, true)) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to initialize callback store.");
        return 2;
    }

    lua_newtable(L);
    lua_newtable(L);
    uint32_t event_count = (uint32_t)lua_rawlen(L, 2);
    for (uint32_t i = 0; i < event_count; ++i) {
        lua_rawgeti(L, 2, (lua_Integer)(i + 1));
        const char *event = luaL_checkstring(L, -1);
        lua_pop(L, 1);
        lua_pushstring(L, event);
        lua_pushboolean(L, 1);
        lua_settable(L, -3);
    }
    lua_setfield(L, -2, "events");

    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "callback");

    lua_Integer cb_count = (lua_Integer)lua_rawlen(L, -2);
    lua_rawseti(L, -2, cb_count + 1);
    lua_pop(L, 1);

    if (!rift_start_auto_pump(L, client)) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to start auto-pump timer.");
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_rift_unsubscribe(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    const char *event = luaL_checkstring(L, 2);

    if (client->server_port == MACH_PORT_NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Client is disconnected.");
        return 2;
    }

    if (client->event_port == MACH_PORT_NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "No active event stream port.");
        return 2;
    }

    return rift_send_event_subscription_request(L, client, "unsubscribe", event);
}

static int l_rift_receive_event(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");

    if (client->event_port == MACH_PORT_NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "No active event stream. Call subscribe first.");
        return 2;
    }

    mach_msg_timeout_t timeout_ms = MACH_MSG_TIMEOUT_NONE;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        lua_Integer v = luaL_checkinteger(L, 2);
        if (v < 0) v = 0;
        timeout_ms = (mach_msg_timeout_t)v;
    }

    bool timed_out = false;
    char *event_json = rift_receive_event_internal(client->event_port, timeout_ms, &timed_out);
    if (!event_json) {
        if (timed_out) {
            lua_pushnil(L);
            return 1;
        }

        lua_pushnil(L);
        lua_pushstring(L, "Failed to receive event.");
        return 2;
    }

    bool res = json_to_lua_table(L, event_json);
    free(event_json);
    if (!res) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to parse event JSON.");
        return 2;
    }

    return 1;
}

static int l_rift_pump(lua_State *L) {
    rift_t *client = (rift_t*)luaL_checkudata(L, 1, "rift.client");
    if (client->event_port == MACH_PORT_NULL) {
        lua_pushinteger(L, 0);
        return 1;
    }

    mach_msg_timeout_t timeout_ms = 0;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        lua_Integer v = luaL_checkinteger(L, 2);
        if (v < 0) v = 0;
        timeout_ms = (mach_msg_timeout_t)v;
    }

    int rc = rift_pump_once_internal(L, client, timeout_ms, true);
    if (rc < 0) return 2;
    lua_pushinteger(L, rc);
    return 1;
}


static const struct luaL_Reg rift_lib[] = {
    {"connect", l_rift_connect},
    {"reconnect", l_rift_reconnect},
    {"send_request", l_rift_send_request},
    {"subscribe", l_rift_subscribe},
    {"unsubscribe", l_rift_unsubscribe},
    {"receive_event", l_rift_receive_event},
    {"pump", l_rift_pump},
    {"disconnect", l_rift_disconnect},
    {NULL, NULL}
};

static const struct luaL_Reg rift_client_methods[] = {
    {"reconnect", l_rift_reconnect},
    {"send_request", l_rift_send_request},
    {"subscribe", l_rift_subscribe},
    {"unsubscribe", l_rift_unsubscribe},
    {"receive_event", l_rift_receive_event},
    {"pump", l_rift_pump},
    {"disconnect", l_rift_disconnect},
    {NULL, NULL}
};

int luaopen_rift(lua_State *L) {
    luaL_newlib(L, rift_lib);

    if (luaL_newmetatable(L, "rift.client")) {
        lua_pushcfunction(L, l_rift_gc);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        luaL_setfuncs(L, rift_client_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    return 1;
}
