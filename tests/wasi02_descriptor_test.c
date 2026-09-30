#include "../src/wasi02_descriptor.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static void assert_v028(turbowasm_wasi02_version version) {
    assert(version.major == 0u);
    assert(version.minor == 2u);
    assert(version.patch == 8u);
}

static void test_wall_clock(void) {
    const turbowasm_wasi02_interface_desc *iface =
        turbowasm_wasi02_find_interface(
            "wasi:clocks", "wall-clock");
    const turbowasm_wasi02_function_desc *now;

    assert(iface != NULL);
    assert_v028(iface->version);
    assert(strcmp(
        iface->source_commit,
        "71e486b1b44a49687dbe17d5b979d6da6112c7f2") == 0);
    assert(iface->function_count == 2u);

    now = turbowasm_wasi02_find_function(iface, "now");
    assert(now != NULL);
    assert(now->param_count == 0u);
    assert(now->result != NULL);
    assert(now->result->kind == TURBOWASM_WASI02_TYPE_RECORD);
    assert(strcmp(now->result->name, "datetime") == 0);
    assert(now->result->as.record.count == 2u);
    assert(strcmp(
        now->result->as.record.fields[0].name,
        "seconds") == 0);
    assert(now->result->as.record.fields[0].type->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(strcmp(
        now->result->as.record.fields[1].name,
        "nanoseconds") == 0);
    assert(now->result->as.record.fields[1].type->kind ==
           TURBOWASM_WASI02_TYPE_U32);
}

static void test_monotonic_pollable(void) {
    const turbowasm_wasi02_interface_desc *iface =
        turbowasm_wasi02_find_interface(
            "wasi:clocks", "monotonic-clock");
    const turbowasm_wasi02_function_desc *subscribe;
    const turbowasm_wasi02_type_desc *pollable;

    assert(iface != NULL);
    assert_v028(iface->version);
    assert(iface->function_count == 4u);

    subscribe = turbowasm_wasi02_find_function(
        iface, "subscribe-duration");
    assert(subscribe != NULL);
    assert(subscribe->param_count == 1u);
    assert(subscribe->params[0].type->kind ==
           TURBOWASM_WASI02_TYPE_ALIAS);
    assert(strcmp(
        subscribe->params[0].type->name,
        "duration") == 0);
    assert(subscribe->params[0].type->as.alias.target->kind ==
           TURBOWASM_WASI02_TYPE_U64);

    pollable = subscribe->result;
    assert(pollable != NULL);
    assert(pollable->kind == TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        pollable->as.resource.package_name,
        "wasi:io") == 0);
    assert(strcmp(
        pollable->as.resource.interface_name,
        "poll") == 0);
    assert(strcmp(
        pollable->as.resource.resource_name,
        "pollable") == 0);
    assert_v028(pollable->as.resource.version);
}

static void test_random(void) {
    const turbowasm_wasi02_interface_desc *random =
        turbowasm_wasi02_find_interface(
            "wasi:random", "random");
    const turbowasm_wasi02_interface_desc *seed =
        turbowasm_wasi02_find_interface(
            "wasi:random", "insecure-seed");
    const turbowasm_wasi02_function_desc *bytes;
    const turbowasm_wasi02_function_desc *seed_fn;

    assert(random != NULL);
    assert_v028(random->version);
    assert(strcmp(
        random->source_commit,
        "bd54965b22082b3e157b2fb4bc77987c33ae7d5f") == 0);

    bytes = turbowasm_wasi02_find_function(
        random, "get-random-bytes");
    assert(bytes != NULL);
    assert(bytes->param_count == 1u);
    assert(bytes->params[0].type->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(bytes->result->kind ==
           TURBOWASM_WASI02_TYPE_LIST);
    assert(bytes->result->as.list.element->kind ==
           TURBOWASM_WASI02_TYPE_U8);

    assert(seed != NULL);
    seed_fn = turbowasm_wasi02_find_function(
        seed, "insecure-seed");
    assert(seed_fn != NULL);
    assert(seed_fn->result->kind ==
           TURBOWASM_WASI02_TYPE_TUPLE);
    assert(seed_fn->result->as.tuple.count == 2u);
    assert(seed_fn->result->as.tuple.elements[0]->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(seed_fn->result->as.tuple.elements[1]->kind ==
           TURBOWASM_WASI02_TYPE_U64);
}

static void test_cli(void) {
    const turbowasm_wasi02_interface_desc *environment =
        turbowasm_wasi02_find_interface(
            "wasi:cli", "environment");
    const turbowasm_wasi02_interface_desc *exit_iface =
        turbowasm_wasi02_find_interface(
            "wasi:cli", "exit");
    const turbowasm_wasi02_function_desc *get_env;
    const turbowasm_wasi02_function_desc *cwd;
    const turbowasm_wasi02_function_desc *exit_fn;

    assert(environment != NULL);
    assert_v028(environment->version);
    assert(strcmp(
        environment->source_commit,
        "e922fd7bd137cd284a5e6c4815a5a630d32fdd01") == 0);

    get_env = turbowasm_wasi02_find_function(
        environment, "get-environment");
    assert(get_env != NULL);
    assert(get_env->result->kind ==
           TURBOWASM_WASI02_TYPE_LIST);
    assert(get_env->result->as.list.element->kind ==
           TURBOWASM_WASI02_TYPE_TUPLE);
    assert(get_env->result->as.list.element->as.tuple.count == 2u);
    assert(get_env->result->as.list.element
               ->as.tuple.elements[0]->kind ==
           TURBOWASM_WASI02_TYPE_STRING);
    assert(get_env->result->as.list.element
               ->as.tuple.elements[1]->kind ==
           TURBOWASM_WASI02_TYPE_STRING);

    cwd = turbowasm_wasi02_find_function(
        environment, "initial-cwd");
    assert(cwd != NULL);
    assert(cwd->result->kind ==
           TURBOWASM_WASI02_TYPE_OPTION);
    assert(cwd->result->as.option.payload->kind ==
           TURBOWASM_WASI02_TYPE_STRING);

    assert(exit_iface != NULL);
    exit_fn = turbowasm_wasi02_find_function(
        exit_iface, "exit");
    assert(exit_fn != NULL);
    assert(exit_fn->param_count == 1u);
    assert(exit_fn->params[0].type->kind ==
           TURBOWASM_WASI02_TYPE_RESULT);
    assert(exit_fn->params[0].type->as.result.ok == NULL);
    assert(exit_fn->params[0].type->as.result.error == NULL);
    assert(exit_fn->result == NULL);

    /* unstable cli-exit-with-code is deliberately outside W1 stable registry */
    assert(turbowasm_wasi02_find_function(
               exit_iface, "exit-with-code") == NULL);
}

static void test_filesystem_preopens(void) {
    const turbowasm_wasi02_interface_desc *preopens =
        turbowasm_wasi02_find_interface(
            "wasi:filesystem", "preopens");
    const turbowasm_wasi02_function_desc *get_directories;
    const turbowasm_wasi02_type_desc *pair;
    const turbowasm_wasi02_type_desc *descriptor;

    assert(preopens != NULL);
    assert_v028(preopens->version);
    assert(strcmp(
        preopens->source_commit,
        "971b11617b50e7496bea85f36e60141bda172964") == 0);
    assert(preopens->function_count == 1u);

    get_directories = turbowasm_wasi02_find_function(
        preopens, "get-directories");
    assert(get_directories != NULL);
    assert(get_directories->param_count == 0u);
    assert(get_directories->result != NULL);
    assert(get_directories->result->kind ==
           TURBOWASM_WASI02_TYPE_LIST);

    pair = get_directories->result->as.list.element;
    assert(pair != NULL);
    assert(pair->kind == TURBOWASM_WASI02_TYPE_TUPLE);
    assert(pair->as.tuple.count == 2u);

    descriptor = pair->as.tuple.elements[0];
    assert(descriptor != NULL);
    assert(descriptor->kind ==
           TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        descriptor->as.resource.package_name,
        "wasi:filesystem") == 0);
    assert(strcmp(
        descriptor->as.resource.interface_name,
        "types") == 0);
    assert(strcmp(
        descriptor->as.resource.resource_name,
        "descriptor") == 0);
    assert_v028(descriptor->as.resource.version);

    assert(pair->as.tuple.elements[1]->kind ==
           TURBOWASM_WASI02_TYPE_STRING);
}

static void test_filesystem_types_surface(void) {
    const turbowasm_wasi02_interface_desc *types =
        turbowasm_wasi02_find_interface(
            "wasi:filesystem", "types");
    const turbowasm_wasi02_function_desc *stat;
    const turbowasm_wasi02_function_desc *stat_at;
    const turbowasm_wasi02_function_desc *open_at;
    const turbowasm_wasi02_function_desc *create_dir;
    const turbowasm_wasi02_function_desc *remove_dir;
    const turbowasm_wasi02_function_desc *unlink_file;
    const turbowasm_wasi02_type_desc *stat_result;
    const turbowasm_wasi02_type_desc *stat_record;
    const turbowasm_wasi02_type_desc *error_code;
    const turbowasm_wasi02_type_desc *path_flags;
    const turbowasm_wasi02_type_desc *open_flags;
    const turbowasm_wasi02_type_desc *descriptor_flags;

    assert(types != NULL);
    assert_v028(types->version);
    assert(strcmp(
        types->source_commit,
        "971b11617b50e7496bea85f36e60141bda172964") == 0);
    assert(types->function_count == 6u);

    stat = turbowasm_wasi02_find_function(
        types, "[method]descriptor.stat");
    stat_at = turbowasm_wasi02_find_function(
        types, "[method]descriptor.stat-at");
    open_at = turbowasm_wasi02_find_function(
        types, "[method]descriptor.open-at");
    create_dir = turbowasm_wasi02_find_function(
        types, "[method]descriptor.create-directory-at");
    remove_dir = turbowasm_wasi02_find_function(
        types, "[method]descriptor.remove-directory-at");
    unlink_file = turbowasm_wasi02_find_function(
        types, "[method]descriptor.unlink-file-at");

    assert(stat != NULL);
    assert(stat->param_count == 1u);
    assert(strcmp(stat->params[0].name, "self") == 0);
    assert(stat->params[0].type->kind ==
           TURBOWASM_WASI02_TYPE_RESOURCE);

    stat_result = stat->result;
    assert(stat_result != NULL);
    assert(stat_result->kind == TURBOWASM_WASI02_TYPE_RESULT);
    stat_record = stat_result->as.result.ok;
    error_code = stat_result->as.result.error;
    assert(stat_record != NULL);
    assert(stat_record->kind == TURBOWASM_WASI02_TYPE_RECORD);
    assert(strcmp(stat_record->name, "descriptor-stat") == 0);
    assert(stat_record->as.record.count == 6u);
    assert(strcmp(
        stat_record->as.record.fields[0].name, "type") == 0);
    assert(stat_record->as.record.fields[0].type->kind ==
           TURBOWASM_WASI02_TYPE_ENUM);
    assert(stat_record->as.record.fields[0]
               .type->as.enumeration.count == 8u);
    assert(strcmp(
        stat_record->as.record.fields[0]
            .type->as.enumeration.labels[6],
        "regular-file") == 0);

    assert(error_code != NULL);
    assert(error_code->kind == TURBOWASM_WASI02_TYPE_ENUM);
    assert(strcmp(error_code->name, "error-code") == 0);
    assert(error_code->as.enumeration.count == 37u);
    assert(strcmp(
        error_code->as.enumeration.labels[31],
        "not-permitted") == 0);
    assert(strcmp(
        error_code->as.enumeration.labels[36],
        "cross-device") == 0);

    assert(stat_at != NULL);
    assert(stat_at->param_count == 3u);
    path_flags = stat_at->params[1].type;
    assert(path_flags->kind == TURBOWASM_WASI02_TYPE_FLAGS);
    assert(path_flags->as.flags.count == 1u);
    assert(strcmp(path_flags->as.flags.labels[0],
                  "symlink-follow") == 0);

    assert(open_at != NULL);
    assert(open_at->param_count == 5u);
    open_flags = open_at->params[3].type;
    descriptor_flags = open_at->params[4].type;
    assert(open_flags->kind == TURBOWASM_WASI02_TYPE_FLAGS);
    assert(open_flags->as.flags.count == 4u);
    assert(strcmp(open_flags->as.flags.labels[3],
                  "truncate") == 0);
    assert(descriptor_flags->kind ==
           TURBOWASM_WASI02_TYPE_FLAGS);
    assert(descriptor_flags->as.flags.count == 6u);
    assert(strcmp(descriptor_flags->as.flags.labels[5],
                  "mutate-directory") == 0);
    assert(open_at->result->kind ==
           TURBOWASM_WASI02_TYPE_RESULT);
    assert(open_at->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(open_at->result->as.result.error == error_code);

    assert(create_dir != NULL);
    assert(remove_dir != NULL);
    assert(unlink_file != NULL);
    assert(create_dir->result->kind ==
           TURBOWASM_WASI02_TYPE_RESULT);
    assert(create_dir->result->as.result.ok == NULL);
    assert(create_dir->result->as.result.error == error_code);
    assert(remove_dir->param_count == 2u);
    assert(unlink_file->param_count == 2u);
}

static void test_io_poll_surface(void) {
    const turbowasm_wasi02_interface_desc *poll =
        turbowasm_wasi02_find_interface(
            "wasi:io", "poll");
    const turbowasm_wasi02_function_desc *ready;
    const turbowasm_wasi02_function_desc *block;
    const turbowasm_wasi02_function_desc *poll_fn;
    const turbowasm_wasi02_type_desc *pollable;

    assert(poll != NULL);
    assert_v028(poll->version);
    assert(strcmp(
        poll->source_commit,
        "3983fe1feab6b3a3b4e5c47c8b13daaf22266f00") == 0);
    assert(poll->function_count == 3u);

    ready = turbowasm_wasi02_find_function(
        poll, "[method]pollable.ready");
    block = turbowasm_wasi02_find_function(
        poll, "[method]pollable.block");
    poll_fn = turbowasm_wasi02_find_function(
        poll, "poll");

    assert(ready != NULL);
    assert(ready->param_count == 1u);
    pollable = ready->params[0].type;
    assert(pollable != NULL);
    assert(pollable->kind ==
           TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        pollable->as.resource.package_name,
        "wasi:io") == 0);
    assert(strcmp(
        pollable->as.resource.interface_name,
        "poll") == 0);
    assert(strcmp(
        pollable->as.resource.resource_name,
        "pollable") == 0);
    assert(ready->result != NULL);
    assert(ready->result->kind ==
           TURBOWASM_WASI02_TYPE_BOOL);

    assert(block != NULL);
    assert(block->param_count == 1u);
    assert(block->params[0].type == pollable);
    assert(block->result == NULL);

    assert(poll_fn != NULL);
    assert(poll_fn->param_count == 1u);
    assert(poll_fn->params[0].type->kind ==
           TURBOWASM_WASI02_TYPE_LIST);
    assert(poll_fn->params[0].type->as.list.element ==
           pollable);
    assert(poll_fn->result != NULL);
    assert(poll_fn->result->kind ==
           TURBOWASM_WASI02_TYPE_LIST);
    assert(poll_fn->result->as.list.element->kind ==
           TURBOWASM_WASI02_TYPE_U32);
}

static void test_io_streams_surface(void) {
    const turbowasm_wasi02_interface_desc *error_iface =
        turbowasm_wasi02_find_interface(
            "wasi:io", "error");
    const turbowasm_wasi02_interface_desc *streams =
        turbowasm_wasi02_find_interface(
            "wasi:io", "streams");
    const turbowasm_wasi02_function_desc *debug;
    const turbowasm_wasi02_function_desc *read;
    const turbowasm_wasi02_function_desc *blocking_read;
    const turbowasm_wasi02_function_desc *skip;
    const turbowasm_wasi02_function_desc *subscribe_in;
    const turbowasm_wasi02_function_desc *check_write;
    const turbowasm_wasi02_function_desc *write;
    const turbowasm_wasi02_function_desc *blocking_write;
    const turbowasm_wasi02_function_desc *flush;
    const turbowasm_wasi02_function_desc *blocking_flush;
    const turbowasm_wasi02_function_desc *subscribe_out;
    const turbowasm_wasi02_function_desc *write_zeroes;
    const turbowasm_wasi02_function_desc *blocking_zeroes;
    const turbowasm_wasi02_function_desc *splice;
    const turbowasm_wasi02_function_desc *blocking_splice;
    const turbowasm_wasi02_type_desc *input_stream;
    const turbowasm_wasi02_type_desc *output_stream;
    const turbowasm_wasi02_type_desc *stream_error;
    const turbowasm_wasi02_type_desc *io_error;
    const turbowasm_wasi02_type_desc *pollable;

    assert(error_iface != NULL);
    assert_v028(error_iface->version);
    assert(strcmp(
        error_iface->source_commit,
        "3983fe1feab6b3a3b4e5c47c8b13daaf22266f00") == 0);
    assert(error_iface->function_count == 1u);

    debug = turbowasm_wasi02_find_function(
        error_iface, "[method]error.to-debug-string");
    assert(debug != NULL);
    assert(debug->param_count == 1u);
    io_error = debug->params[0].type;
    assert(io_error != NULL);
    assert(io_error->kind == TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(io_error->as.resource.package_name, "wasi:io") == 0);
    assert(strcmp(io_error->as.resource.interface_name, "error") == 0);
    assert(strcmp(io_error->as.resource.resource_name, "error") == 0);
    assert(debug->result != NULL);
    assert(debug->result->kind == TURBOWASM_WASI02_TYPE_STRING);

    assert(streams != NULL);
    assert_v028(streams->version);
    assert(strcmp(
        streams->source_commit,
        "3983fe1feab6b3a3b4e5c47c8b13daaf22266f00") == 0);
    assert(streams->function_count == 15u);

    read = turbowasm_wasi02_find_function(
        streams, "[method]input-stream.read");
    blocking_read = turbowasm_wasi02_find_function(
        streams, "[method]input-stream.blocking-read");
    skip = turbowasm_wasi02_find_function(
        streams, "[method]input-stream.skip");
    subscribe_in = turbowasm_wasi02_find_function(
        streams, "[method]input-stream.subscribe");
    check_write = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.check-write");
    write = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.write");
    blocking_write = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.blocking-write-and-flush");
    flush = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.flush");
    blocking_flush = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.blocking-flush");
    subscribe_out = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.subscribe");
    write_zeroes = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.write-zeroes");
    blocking_zeroes = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.blocking-write-zeroes-and-flush");
    splice = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.splice");
    blocking_splice = turbowasm_wasi02_find_function(
        streams, "[method]output-stream.blocking-splice");

    assert(read != NULL);
    assert(blocking_read != NULL);
    assert(skip != NULL);
    assert(subscribe_in != NULL);
    assert(check_write != NULL);
    assert(write != NULL);
    assert(blocking_write != NULL);
    assert(flush != NULL);
    assert(blocking_flush != NULL);
    assert(subscribe_out != NULL);
    assert(write_zeroes != NULL);
    assert(blocking_zeroes != NULL);
    assert(splice != NULL);
    assert(blocking_splice != NULL);

    input_stream = read->params[0].type;
    assert(input_stream != NULL);
    assert(input_stream->kind == TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        input_stream->as.resource.interface_name,
        "streams") == 0);
    assert(strcmp(
        input_stream->as.resource.resource_name,
        "input-stream") == 0);
    assert(read->params[1].type->kind ==
           TURBOWASM_WASI02_TYPE_U64);

    assert(read->result != NULL);
    assert(read->result->kind == TURBOWASM_WASI02_TYPE_RESULT);
    assert(read->result->as.result.ok != NULL);
    assert(read->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_LIST);
    assert(read->result->as.result.ok->as.list.element->kind ==
           TURBOWASM_WASI02_TYPE_U8);

    stream_error = read->result->as.result.error;
    assert(stream_error != NULL);
    assert(stream_error->kind == TURBOWASM_WASI02_TYPE_VARIANT);
    assert(strcmp(stream_error->name, "stream-error") == 0);
    assert(stream_error->as.variant.count == 2u);
    assert(strcmp(
        stream_error->as.variant.cases[0].name,
        "last-operation-failed") == 0);
    assert(stream_error->as.variant.cases[0].payload == io_error);
    assert(strcmp(
        stream_error->as.variant.cases[1].name,
        "closed") == 0);
    assert(stream_error->as.variant.cases[1].payload == NULL);

    assert(blocking_read->result == read->result);
    assert(skip->result->kind == TURBOWASM_WASI02_TYPE_RESULT);
    assert(skip->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(skip->result->as.result.error == stream_error);

    pollable = subscribe_in->result;
    assert(pollable != NULL);
    assert(pollable->kind == TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        pollable->as.resource.interface_name,
        "poll") == 0);
    assert(strcmp(
        pollable->as.resource.resource_name,
        "pollable") == 0);

    output_stream = check_write->params[0].type;
    assert(output_stream != NULL);
    assert(output_stream->kind == TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        output_stream->as.resource.resource_name,
        "output-stream") == 0);
    assert(check_write->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(check_write->result->as.result.error == stream_error);

    assert(write->params[1].type->kind ==
           TURBOWASM_WASI02_TYPE_LIST);
    assert(write->params[1].type->as.list.element->kind ==
           TURBOWASM_WASI02_TYPE_U8);
    assert(write->result->as.result.ok == NULL);
    assert(write->result->as.result.error == stream_error);
    assert(blocking_write->result == write->result);
    assert(flush->result == write->result);
    assert(blocking_flush->result == write->result);
    assert(subscribe_out->result == pollable);
    assert(write_zeroes->params[1].type->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(blocking_zeroes->result == write->result);

    assert(splice->param_count == 3u);
    assert(splice->params[0].type == output_stream);
    assert(splice->params[1].type == input_stream);
    assert(splice->params[2].type->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(splice->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_U64);
    assert(splice->result->as.result.error == stream_error);
    assert(blocking_splice->result == splice->result);
}

int main(void) {
    size_t i;

    assert(turbowasm_wasi02_interface_count() == 12u);
    for (i = 0u; i < turbowasm_wasi02_interface_count(); ++i) {
        const turbowasm_wasi02_interface_desc *iface =
            turbowasm_wasi02_interface_at(i);
        assert(iface != NULL);
        assert(iface->package_name != NULL);
        assert(iface->interface_name != NULL);
        assert(iface->source_repository != NULL);
        assert(iface->source_commit != NULL);
        assert_v028(iface->version);
    }
    assert(turbowasm_wasi02_interface_at(
               turbowasm_wasi02_interface_count()) == NULL);

    test_wall_clock();
    test_monotonic_pollable();
    test_random();
    test_cli();
    test_filesystem_preopens();
    test_filesystem_types_surface();
    test_io_poll_surface();
    test_io_streams_surface();
    return 0;
}
