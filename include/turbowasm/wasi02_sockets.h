#ifndef TURBOWASM_WASI02_SOCKETS_PUBLIC_H
#define TURBOWASM_WASI02_SOCKETS_PUBLIC_H

#include <turbowasm/status.h>
#include <turbowasm/value.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable wasi:sockets@0.2.8 network.error-code values.
 * NONE is an internal success sentinel; the remaining values preserve the
 * exact WIT enum order so error_code - 1 is the descriptor enum index.
 */
typedef enum turbowasm_wasi02_socket_error {
    TURBOWASM_WASI02_SOCKET_ERROR_NONE = 0,
    TURBOWASM_WASI02_SOCKET_ERROR_UNKNOWN,
    TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED,
    TURBOWASM_WASI02_SOCKET_ERROR_NOT_SUPPORTED,
    TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT,
    TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY,
    TURBOWASM_WASI02_SOCKET_ERROR_TIMEOUT,
    TURBOWASM_WASI02_SOCKET_ERROR_CONCURRENCY_CONFLICT,
    TURBOWASM_WASI02_SOCKET_ERROR_NOT_IN_PROGRESS,
    TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK,
    TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE,
    TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT,
    TURBOWASM_WASI02_SOCKET_ERROR_ADDRESS_NOT_BINDABLE,
    TURBOWASM_WASI02_SOCKET_ERROR_ADDRESS_IN_USE,
    TURBOWASM_WASI02_SOCKET_ERROR_REMOTE_UNREACHABLE,
    TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_REFUSED,
    TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_RESET,
    TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_ABORTED,
    TURBOWASM_WASI02_SOCKET_ERROR_DATAGRAM_TOO_LARGE,
    TURBOWASM_WASI02_SOCKET_ERROR_NAME_UNRESOLVABLE,
    TURBOWASM_WASI02_SOCKET_ERROR_TEMPORARY_RESOLVER_FAILURE,
    TURBOWASM_WASI02_SOCKET_ERROR_PERMANENT_RESOLVER_FAILURE
} turbowasm_wasi02_socket_error;

typedef enum turbowasm_wasi02_ip_address_family {
    TURBOWASM_WASI02_IP_ADDRESS_IPV4 = 0,
    TURBOWASM_WASI02_IP_ADDRESS_IPV6 = 1
} turbowasm_wasi02_ip_address_family;

typedef struct turbowasm_wasi02_ipv4_socket_address {
    uint16_t port;
    uint8_t address[4];
} turbowasm_wasi02_ipv4_socket_address;

typedef struct turbowasm_wasi02_ipv6_socket_address {
    uint16_t port;
    uint32_t flow_info;
    uint16_t address[8];
    uint32_t scope_id;
} turbowasm_wasi02_ipv6_socket_address;

typedef struct turbowasm_wasi02_ip_socket_address {
    turbowasm_wasi02_ip_address_family family;
    union {
        turbowasm_wasi02_ipv4_socket_address ipv4;
        turbowasm_wasi02_ipv6_socket_address ipv6;
    } as;
} turbowasm_wasi02_ip_socket_address;

typedef enum turbowasm_wasi02_tcp_shutdown_type {
    TURBOWASM_WASI02_TCP_SHUTDOWN_RECEIVE = 0,
    TURBOWASM_WASI02_TCP_SHUTDOWN_SEND,
    TURBOWASM_WASI02_TCP_SHUTDOWN_BOTH
} turbowasm_wasi02_tcp_shutdown_type;

typedef turbowasm_status (*turbowasm_wasi02_instance_network_fn)(
    void *context,
    turbowasm_value *out_network_rep);

typedef turbowasm_status (*turbowasm_wasi02_socket_drop_fn)(
    void *context,
    turbowasm_value rep);

typedef turbowasm_status (*turbowasm_wasi02_tcp_create_fn)(
    void *context,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out_socket_rep,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_start_bind_fn)(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value network_rep,
    const turbowasm_wasi02_ip_socket_address *local_address,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_start_connect_fn)(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value network_rep,
    const turbowasm_wasi02_ip_socket_address *remote_address,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_operation_fn)(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_finish_connect_fn)(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value *out_input_stream_rep,
    turbowasm_value *out_output_stream_rep,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_accept_fn)(
    void *context,
    turbowasm_value listener_rep,
    turbowasm_value *out_socket_rep,
    turbowasm_value *out_input_stream_rep,
    turbowasm_value *out_output_stream_rep,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_get_address_fn)(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_ip_socket_address *out_address,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_get_bool_fn)(
    void *context,
    turbowasm_value socket_rep,
    bool *out_value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_set_bool_fn)(
    void *context,
    turbowasm_value socket_rep,
    bool value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_get_u8_fn)(
    void *context,
    turbowasm_value socket_rep,
    uint8_t *out_value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_set_u8_fn)(
    void *context,
    turbowasm_value socket_rep,
    uint8_t value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_get_u32_fn)(
    void *context,
    turbowasm_value socket_rep,
    uint32_t *out_value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_set_u32_fn)(
    void *context,
    turbowasm_value socket_rep,
    uint32_t value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_get_u64_fn)(
    void *context,
    turbowasm_value socket_rep,
    uint64_t *out_value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_set_u64_fn)(
    void *context,
    turbowasm_value socket_rep,
    uint64_t value,
    turbowasm_wasi02_socket_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_tcp_subscribe_fn)(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_value *out_pollable_rep);

typedef turbowasm_status (*turbowasm_wasi02_tcp_shutdown_fn)(
    void *context,
    turbowasm_value socket_rep,
    turbowasm_wasi02_tcp_shutdown_type how,
    turbowasm_wasi02_socket_error *out_error);

typedef struct turbowasm_wasi02_socket_provider {
    void *context;

    turbowasm_wasi02_instance_network_fn instance_network;
    turbowasm_wasi02_socket_drop_fn network_drop;

    turbowasm_wasi02_tcp_create_fn tcp_create;
    turbowasm_wasi02_socket_drop_fn tcp_drop;
    turbowasm_wasi02_tcp_start_bind_fn tcp_start_bind;
    turbowasm_wasi02_tcp_operation_fn tcp_finish_bind;
    turbowasm_wasi02_tcp_start_connect_fn tcp_start_connect;
    turbowasm_wasi02_tcp_finish_connect_fn tcp_finish_connect;
    turbowasm_wasi02_tcp_operation_fn tcp_start_listen;
    turbowasm_wasi02_tcp_operation_fn tcp_finish_listen;
    turbowasm_wasi02_tcp_accept_fn tcp_accept;

    turbowasm_wasi02_tcp_get_address_fn tcp_local_address;
    turbowasm_wasi02_tcp_get_address_fn tcp_remote_address;

    turbowasm_wasi02_tcp_set_u64_fn tcp_set_listen_backlog_size;
    turbowasm_wasi02_tcp_get_bool_fn tcp_keep_alive_enabled;
    turbowasm_wasi02_tcp_set_bool_fn tcp_set_keep_alive_enabled;
    turbowasm_wasi02_tcp_get_u64_fn tcp_keep_alive_idle_time;
    turbowasm_wasi02_tcp_set_u64_fn tcp_set_keep_alive_idle_time;
    turbowasm_wasi02_tcp_get_u64_fn tcp_keep_alive_interval;
    turbowasm_wasi02_tcp_set_u64_fn tcp_set_keep_alive_interval;
    turbowasm_wasi02_tcp_get_u32_fn tcp_keep_alive_count;
    turbowasm_wasi02_tcp_set_u32_fn tcp_set_keep_alive_count;
    turbowasm_wasi02_tcp_get_u8_fn tcp_hop_limit;
    turbowasm_wasi02_tcp_set_u8_fn tcp_set_hop_limit;
    turbowasm_wasi02_tcp_get_u64_fn tcp_receive_buffer_size;
    turbowasm_wasi02_tcp_set_u64_fn tcp_set_receive_buffer_size;
    turbowasm_wasi02_tcp_get_u64_fn tcp_send_buffer_size;
    turbowasm_wasi02_tcp_set_u64_fn tcp_set_send_buffer_size;

    turbowasm_wasi02_tcp_subscribe_fn tcp_subscribe;
    turbowasm_wasi02_tcp_shutdown_fn tcp_shutdown;
} turbowasm_wasi02_socket_provider;

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI02_SOCKETS_PUBLIC_H */
