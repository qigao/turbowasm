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

int main(void) {
    size_t i;

    assert(turbowasm_wasi02_interface_count() == 7u);
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
    return 0;
}
