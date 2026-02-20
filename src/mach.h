#include <mach/mach.h>
#include <bootstrap.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>


#define RIFT_SERVICE_NAME "git.acsandmann.rift"
#define MAX_MSG_SIZE (64 * 1024)
#define RIFT_EVENT_PORT_QLIMIT MACH_PORT_QLIMIT_LARGE

typedef struct {
    mach_port_t server_port;
    mach_port_t event_port;
} rift_t;

static bool rift_set_port_queue_limit_internal(mach_port_t port, mach_port_msgcount_t qlimit) {
    if (port == MACH_PORT_NULL) {
        return false;
    }

    mach_port_limits_t limits;
    limits.mpl_qlimit = qlimit;
    kern_return_t kr = mach_port_set_attributes(
        mach_task_self(),
        port,
        MACH_PORT_LIMITS_INFO,
        (mach_port_info_t)&limits,
        MACH_PORT_LIMITS_INFO_COUNT
    );

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mach_port_set_attributes failed: %s\n", mach_error_string(kr));
        return false;
    }

    return true;
}

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

static mach_port_t rift_allocate_reply_port_internal() {
    mach_port_t reply_port = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mach_port_allocate failed: %s\n", mach_error_string(kr));
        return MACH_PORT_NULL;
    }

    if (!rift_set_port_queue_limit_internal(reply_port, RIFT_EVENT_PORT_QLIMIT)) {
        mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), reply_port);
        return MACH_PORT_NULL;
    }

    kr = mach_port_insert_right(mach_task_self(), reply_port, reply_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), reply_port);
        fprintf(stderr, "mach_port_insert_right failed: %s\n", mach_error_string(kr));
        return MACH_PORT_NULL;
    }

    return reply_port;
}

static void rift_deallocate_reply_port_internal(mach_port_t reply_port) {
    if (reply_port == MACH_PORT_NULL) return;
    mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), reply_port);
}

static char* rift_send_request_with_reply_port_internal(
    mach_port_t server_port,
    mach_port_t reply_port,
    const char* request_json,
    bool await_response
) {
    if (server_port == MACH_PORT_NULL || reply_port == MACH_PORT_NULL || request_json == NULL) {
        return NULL;
    }

    size_t request_json_len = strlen(request_json) + 1;
    uint32_t aligned_len = (request_json_len + 3) & ~3;
    uint32_t total_size = sizeof(mach_msg_header_t) + aligned_len;

    if (total_size < 64) {
        total_size = 64;
    }

    char* request_buffer = calloc(1, total_size);
    if (!request_buffer) {
        return NULL;
    }

    mach_msg_header_t* request_msg = (mach_msg_header_t*)request_buffer;
    request_msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_COPY_SEND);
    request_msg->msgh_local_port = reply_port;
    request_msg->msgh_remote_port = server_port;
    request_msg->msgh_size = total_size;
    request_msg->msgh_id = (mach_msg_id_t)reply_port;

    memcpy(request_buffer + sizeof(mach_msg_header_t), request_json, request_json_len);

    kern_return_t kr = mach_msg(
        request_msg,
        MACH_SEND_MSG,
        total_size,
        0,
        MACH_PORT_NULL,
        MACH_MSG_TIMEOUT_NONE,
        MACH_PORT_NULL
    );

    free(request_buffer);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mach_msg SEND failed: %s\n", mach_error_string(kr));
        return NULL;
    }

    if (!await_response) return (char*)1;

    char response_buffer[MAX_MSG_SIZE];
    mach_msg_header_t* response_msg = (mach_msg_header_t*)response_buffer;
    kr = mach_msg(response_msg, MACH_RCV_MSG, 0, MAX_MSG_SIZE, reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

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

static char* rift_receive_event_internal_with_options(
    mach_port_t reply_port,
    mach_msg_timeout_t timeout_ms,
    bool use_timeout,
    bool* timed_out
) {
    if (timed_out) *timed_out = false;

    if (reply_port == MACH_PORT_NULL) {
        return NULL;
    }

    char event_buffer[MAX_MSG_SIZE];
    mach_msg_header_t* event_msg = (mach_msg_header_t*)event_buffer;

    mach_msg_option_t options = MACH_RCV_MSG;
    if (use_timeout) {
        options |= MACH_RCV_TIMEOUT;
    }

    kern_return_t kr = mach_msg(
        event_msg,
        options,
        0,
        MAX_MSG_SIZE,
        reply_port,
        use_timeout ? timeout_ms : MACH_MSG_TIMEOUT_NONE,
        MACH_PORT_NULL
    );

    if (kr != KERN_SUCCESS) {
        if (kr == MACH_RCV_TIMED_OUT && timed_out) {
            *timed_out = true;
            return NULL;
        }
        fprintf(stderr, "mach_msg RECEIVE event failed: %s\n", mach_error_string(kr));
        return NULL;
    }

    char* event_json_ptr = (char*)event_msg + sizeof(mach_msg_header_t);
    size_t event_len = event_msg->msgh_size - sizeof(mach_msg_header_t);
    char* result = (char*)malloc(event_len + 1);
    if (result) {
        memcpy(result, event_json_ptr, event_len);
        result[event_len] = '\0';
    }

    return result;
}

static char* rift_receive_event_internal(mach_port_t reply_port, mach_msg_timeout_t timeout_ms, bool* timed_out) {
    return rift_receive_event_internal_with_options(reply_port, timeout_ms, timeout_ms > 0, timed_out);
}

static char* rift_try_receive_event_internal(mach_port_t reply_port, mach_msg_timeout_t timeout_ms, bool* timed_out) {
    return rift_receive_event_internal_with_options(reply_port, timeout_ms, true, timed_out);
}

static void rift_disconnect_internal(mach_port_t server_port) {
    if (server_port != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), server_port);
    }
}
