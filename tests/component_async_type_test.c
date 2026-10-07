#include "component_type_graph.h"
#include <tinytest.h>

static turbowasm_component_type_graph graph;
static turbowasm_component_type_graph other_graph;

static turbowasm_component_type_ref indexed(uint32_t id) {
    return turbowasm_component_type_ref_indexed(id);
}

static turbowasm_component_type_ref scalar(turbowasm_component_type_kind kind) {
    return turbowasm_component_type_ref_inline(kind);
}

static bool define_wrapper(turbowasm_component_type_graph *target, uint32_t id, turbowasm_component_type_kind kind,
    turbowasm_component_type_ref payload) {
    static const uint8_t name[] = "value";
    turbowasm_component_record_field field = {name, 5u, payload};
    turbowasm_component_variant_case variant = {name, 5u, true, payload};
    switch (kind) {
        case TURBOWASM_COMPONENT_TYPE_LIST:
            return turbowasm_component_type_graph_define_list_ref(target, id, payload);
        case TURBOWASM_COMPONENT_TYPE_RECORD:
            return turbowasm_component_type_graph_define_record(target, id, &field, 1u);
        case TURBOWASM_COMPONENT_TYPE_TUPLE:
            return turbowasm_component_type_graph_define_tuple(target, id, &payload, 1u);
        case TURBOWASM_COMPONENT_TYPE_VARIANT:
            return turbowasm_component_type_graph_define_variant(target, id, &variant, 1u);
        case TURBOWASM_COMPONENT_TYPE_OPTION:
            return turbowasm_component_type_graph_define_option(target, id, payload);
        case TURBOWASM_COMPONENT_TYPE_RESULT:
            return turbowasm_component_type_graph_define_result(
                target, id, false, indexed(UINT32_MAX), true, payload);
        default:
            return turbowasm_component_type_graph_define_async_value(
                target, id, kind, true, payload);
    }
}

static const turbowasm_component_type_kind wrappers[] = {
    TURBOWASM_COMPONENT_TYPE_LIST,
    TURBOWASM_COMPONENT_TYPE_RECORD,
    TURBOWASM_COMPONENT_TYPE_TUPLE,
    TURBOWASM_COMPONENT_TYPE_VARIANT,
    TURBOWASM_COMPONENT_TYPE_OPTION,
    TURBOWASM_COMPONENT_TYPE_RESULT,
    TURBOWASM_COMPONENT_TYPE_FUTURE,
    TURBOWASM_COMPONENT_TYPE_STREAM
};

spec("private Component future and stream types") {
    after_each() {
        turbowasm_component_type_graph_destroy(&graph);
        turbowasm_component_type_graph_destroy(&other_graph);
    }

    it("represents unit endpoints without a payload reference") {
        uint32_t i;
        check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, false, indexed(UINT32_MAX)));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 1u, TURBOWASM_COMPONENT_TYPE_STREAM, false, indexed(UINT32_MAX)));
        check_true(turbowasm_component_type_graph_validate(&graph));
        for (i = 0u; i < 2u; ++i) {
            const turbowasm_component_type *type =
                turbowasm_component_type_graph_get(&graph, i);
            check_not_null(type);
            check_false(type->as.async_value.has_payload);
            check_null(turbowasm_component_scalar_cmeta_type(type->kind));
        }
    }

    it("accepts primitive payloads subject to the pinned stream char restriction") {
        turbowasm_component_type_kind kind;
        check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        for (kind = TURBOWASM_COMPONENT_TYPE_BOOL;
             kind <= TURBOWASM_COMPONENT_TYPE_STRING; ++kind) {
            check_true(turbowasm_component_type_graph_define_async_value(
                &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, true, scalar(kind)));
            check_true(turbowasm_component_type_graph_define_scalar(
                &graph, 1u, TURBOWASM_COMPONENT_TYPE_U32));
            check_true(turbowasm_component_type_graph_validate(&graph));
            turbowasm_component_type_graph_destroy(&graph);
            check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
            check_true(kind == TURBOWASM_COMPONENT_TYPE_STRING
                ? turbowasm_component_type_graph_define_string(&graph, 0u)
                : turbowasm_component_type_graph_define_scalar(&graph, 0u, kind));
            check_true(turbowasm_component_type_graph_define_async_value(
                &graph, 1u, TURBOWASM_COMPONENT_TYPE_STREAM, true, scalar(kind)));
            check_equal(turbowasm_component_type_graph_validate(&graph),
                kind != TURBOWASM_COMPONENT_TYPE_CHAR);
            graph.types[1].as.async_value.payload = indexed(0u);
            check_equal(turbowasm_component_type_graph_validate(&graph),
                kind != TURBOWASM_COMPONENT_TYPE_CHAR);
            turbowasm_component_type_graph_destroy(&graph);
            check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        }
    }

    it("allows char nested inside a stream element record") {
        check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        check_true(define_wrapper(&graph, 0u, TURBOWASM_COMPONENT_TYPE_RECORD,
            scalar(TURBOWASM_COMPONENT_TYPE_CHAR)));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 1u, TURBOWASM_COMPONENT_TYPE_STREAM, true, indexed(0u)));
        check_true(turbowasm_component_type_graph_validate(&graph));
    }

    it("allows nested endpoint and composite payloads owning resources") {
        uint32_t i;
        check_true(turbowasm_component_type_graph_allocate(&graph, 12u));
        check_true(turbowasm_component_type_graph_define_resource(&graph, 0u, 42u));
        check_true(turbowasm_component_type_graph_define_handle(
            &graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
        for (i = 0u; i < sizeof(wrappers) / sizeof(wrappers[0]); ++i)
            check_true(define_wrapper(&graph, i + 2u, wrappers[i], indexed(i + 1u)));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 10u, TURBOWASM_COMPONENT_TYPE_FUTURE, true, indexed(9u)));
        check_true(turbowasm_component_type_graph_define_function(
            &graph, 11u, NULL, 0u, true, indexed(10u)));
        check_true(turbowasm_component_type_graph_validate(&graph));
    }

    it("rejects borrowed payloads through every composite wrapper") {
        uint32_t i;
        for (i = 0u; i < sizeof(wrappers) / sizeof(wrappers[0]); ++i) {
            check_true(turbowasm_component_type_graph_allocate(&graph, 4u));
            check_true(turbowasm_component_type_graph_define_resource(&graph, 0u, 42u));
            check_true(turbowasm_component_type_graph_define_handle(
                &graph, 1u, TURBOWASM_COMPONENT_TYPE_BORROW, 0u));
            check_true(define_wrapper(&graph, 2u, wrappers[i], indexed(1u)));
            check_true(turbowasm_component_type_graph_define_async_value(
                &graph, 3u, TURBOWASM_COMPONENT_TYPE_FUTURE, true, indexed(2u)));
            check_false(turbowasm_component_type_graph_validate(&graph));
            graph.types[3].kind = TURBOWASM_COMPONENT_TYPE_STREAM;
            check_false(turbowasm_component_type_graph_validate(&graph));
            turbowasm_component_type_graph_destroy(&graph);
        }
    }

    it("validates forward references and rejects non-value payloads") {
        check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, true, indexed(1u)));
        check_false(turbowasm_component_type_graph_validate(&graph));
        check_true(turbowasm_component_type_graph_define_scalar(
            &graph, 1u, TURBOWASM_COMPONENT_TYPE_U32));
        check_true(turbowasm_component_type_graph_validate(&graph));
        turbowasm_component_type_graph_destroy(&graph);

        check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_STREAM, true, indexed(1u)));
        check_true(turbowasm_component_type_graph_define_function(
            &graph, 1u, NULL, 0u, false, indexed(UINT32_MAX)));
        check_false(turbowasm_component_type_graph_validate(&graph));
        turbowasm_component_type_graph_destroy(&graph);

        check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, true, indexed(1u)));
        check_true(turbowasm_component_type_graph_define_resource(&graph, 1u, 42u));
        check_false(turbowasm_component_type_graph_validate(&graph));
    }

    it("rejects invalid definitions without consuming the target slot") {
        const turbowasm_component_type *type;
        check_true(turbowasm_component_type_graph_allocate(&graph, 1u));
        check_false(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_OPTION, true,
            scalar(TURBOWASM_COMPONENT_TYPE_U32)));
        check_false(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_STREAM, true, indexed(1u)));
        check_false(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, true,
            scalar(TURBOWASM_COMPONENT_TYPE_RESOURCE)));
        type = turbowasm_component_type_graph_get(&graph, 0u);
        check_equal(type->kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_STREAM, false, indexed(0u)));
        check_false(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, true,
            scalar(TURBOWASM_COMPONENT_TYPE_U32)));
        check_equal(type->kind, TURBOWASM_COMPONENT_TYPE_STREAM);
        check_false(type->as.async_value.has_payload);
        check_true(turbowasm_component_type_graph_validate(&graph));
    }

    it("rejects self cycles and cycles crossing containers") {
        check_true(turbowasm_component_type_graph_allocate(&graph, 1u));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, true, indexed(0u)));
        check_false(turbowasm_component_type_graph_validate(&graph));
        turbowasm_component_type_graph_destroy(&graph);
        check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_STREAM, true, indexed(1u)));
        check_true(turbowasm_component_type_graph_define_list(&graph, 1u, 0u));
        check_false(turbowasm_component_type_graph_validate(&graph));
        check_false(turbowasm_component_value_type_equal(&graph, indexed(0u), &graph, indexed(0u)));
    }

    it("enforces the existing value nesting bound on async payloads") {
        uint32_t count, i;
        for (count = TURBOWASM_COMPONENT_VALUE_MAX_DEPTH;
             count <= TURBOWASM_COMPONENT_VALUE_MAX_DEPTH + 1u; ++count) {
            check_true(turbowasm_component_type_graph_allocate(&graph, count));
            for (i = 0u; i + 1u < count; ++i)
                check_true(turbowasm_component_type_graph_define_async_value(
                    &graph, i, (i % 2u) == 0u ? TURBOWASM_COMPONENT_TYPE_STREAM :
                        TURBOWASM_COMPONENT_TYPE_FUTURE, true, indexed(i + 1u)));
            check_true(turbowasm_component_type_graph_define_scalar(
                &graph, count - 1u, TURBOWASM_COMPONENT_TYPE_U8));
            check_equal(turbowasm_component_type_graph_validate(&graph),
                count == TURBOWASM_COMPONENT_VALUE_MAX_DEPTH);
            check_equal(turbowasm_component_value_type_equal(&graph, indexed(0u), &graph, indexed(0u)),
                count == TURBOWASM_COMPONENT_VALUE_MAX_DEPTH);
            turbowasm_component_type_graph_destroy(&graph);
        }
    }

    it("keeps endpoint metadata outside synchronous host admission") {
        uint32_t i, features = UINT32_MAX;
        check_true(turbowasm_component_type_graph_allocate(&graph, 3u));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 0u, TURBOWASM_COMPONENT_TYPE_FUTURE, true,
            scalar(TURBOWASM_COMPONENT_TYPE_STRING)));
        check_true(turbowasm_component_type_graph_define_async_value(
            &graph, 1u, TURBOWASM_COMPONENT_TYPE_STREAM, false, indexed(UINT32_MAX)));
        check_true(turbowasm_component_type_graph_define_option(&graph, 2u, indexed(0u)));
        check_true(turbowasm_component_type_graph_validate(&graph));
        for (i = 0u; i < 3u; ++i) {
            check_false(turbowasm_component_value_type_features(&graph, indexed(i), &features));
            check_equal(features, UINT32_MAX);
        }
        check_true(turbowasm_component_value_type_features(
            &graph, scalar(TURBOWASM_COMPONENT_TYPE_STRING), &features));
        check_equal(features, TURBOWASM_COMPONENT_VALUE_DYNAMIC_MEMORY);
    }

    it("compares primitive types across inline and independently indexed graphs") {
        turbowasm_component_type_kind a, b;
        check_true(turbowasm_component_type_graph_allocate(&graph, 1u));
        check_true(turbowasm_component_type_graph_define_scalar(&graph, 0u, TURBOWASM_COMPONENT_TYPE_U32));
        for (a = TURBOWASM_COMPONENT_TYPE_BOOL; a <= TURBOWASM_COMPONENT_TYPE_STRING; ++a) {
            graph.types[0].kind = a;
            for (b = TURBOWASM_COMPONENT_TYPE_BOOL; b <= TURBOWASM_COMPONENT_TYPE_STRING; ++b) {
                check_equal(turbowasm_component_value_type_equal(&graph, indexed(0u), &other_graph, scalar(b)), a == b);
                check_equal(turbowasm_component_value_type_equal(&other_graph, scalar(b), &graph, indexed(0u)), a == b);
            }
        }
        check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &graph, indexed(1u)));
        check_false(turbowasm_component_value_type_equal(NULL, scalar(TURBOWASM_COMPONENT_TYPE_U32),
            &graph, indexed(0u)));
    }

    it("compares every composite wrapper structurally without relying on graph indices") {
        uint32_t i;
        for (i = 0u; i < sizeof(wrappers) / sizeof(wrappers[0]); ++i) {
            check_true(turbowasm_component_type_graph_allocate(&graph, 2u));
            check_true(turbowasm_component_type_graph_allocate(&other_graph, 2u));
            check_true(turbowasm_component_type_graph_define_scalar(&graph, 0u, TURBOWASM_COMPONENT_TYPE_U32));
            check_true(turbowasm_component_type_graph_define_scalar(&other_graph, 1u, TURBOWASM_COMPONENT_TYPE_U32));
            check_true(define_wrapper(&graph, 1u, wrappers[i], scalar(TURBOWASM_COMPONENT_TYPE_U32)));
            check_true(define_wrapper(&other_graph, 0u, wrappers[i], indexed(1u)));
            check_true(turbowasm_component_type_graph_validate(&graph));
            check_true(turbowasm_component_type_graph_validate(&other_graph));
            check_true(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
            other_graph.types[1].kind = TURBOWASM_COMPONENT_TYPE_U64;
            check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
            other_graph.types[1].kind = TURBOWASM_COMPONENT_TYPE_U32;
            if (wrappers[i] == TURBOWASM_COMPONENT_TYPE_RECORD) {
                other_graph.types[0].as.record.fields[0].name = (const uint8_t *)"other";
                check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
            } else if (wrappers[i] == TURBOWASM_COMPONENT_TYPE_VARIANT) {
                other_graph.types[0].as.variant.cases[0].has_payload = false;
                check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
                other_graph.types[0].as.variant.cases[0].has_payload = true;
                other_graph.types[0].as.variant.cases[0].name = (const uint8_t *)"other";
                check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
            } else if (wrappers[i] == TURBOWASM_COMPONENT_TYPE_RESULT) {
                other_graph.types[0].as.result.has_error = false;
                check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
            } else if (wrappers[i] == TURBOWASM_COMPONENT_TYPE_FUTURE || wrappers[i] == TURBOWASM_COMPONENT_TYPE_STREAM) {
                other_graph.types[0].as.async_value.has_payload = false;
                check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
                graph.types[1].as.async_value.has_payload = false;
                check_true(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
                other_graph.types[0].kind = wrappers[i] == TURBOWASM_COMPONENT_TYPE_FUTURE
                    ? TURBOWASM_COMPONENT_TYPE_STREAM : TURBOWASM_COMPONENT_TYPE_FUTURE;
                check_false(turbowasm_component_value_type_equal(&graph, indexed(1u), &other_graph, indexed(0u)));
            }
            turbowasm_component_type_graph_destroy(&graph);
            turbowasm_component_type_graph_destroy(&other_graph);
        }
    }

    it("preserves label order and nominal resource identity when comparing payloads") {
        turbowasm_component_label labels[2] = {
            {(const uint8_t *)"a", 1u}, {(const uint8_t *)"b", 1u}
        };
        uint32_t i;
        check_true(turbowasm_component_type_graph_allocate(&graph, 4u));
        check_true(turbowasm_component_type_graph_allocate(&other_graph, 4u));
        check_true(turbowasm_component_type_graph_define_enum(&graph, 0u, labels, 2u));
        check_true(turbowasm_component_type_graph_define_flags(&graph, 1u, labels, 2u));
        check_true(turbowasm_component_type_graph_define_enum(&other_graph, 0u, labels, 2u));
        check_true(turbowasm_component_type_graph_define_flags(&other_graph, 1u, labels, 2u));
        check_true(turbowasm_component_type_graph_define_resource(&graph, 2u, 42u));
        check_true(turbowasm_component_type_graph_define_resource(&other_graph, 2u, 42u));
        check_true(turbowasm_component_type_graph_define_handle(&graph, 3u, TURBOWASM_COMPONENT_TYPE_OWN, 2u));
        check_true(turbowasm_component_type_graph_define_handle(&other_graph, 3u, TURBOWASM_COMPONENT_TYPE_OWN, 2u));
        for (i = 0u; i < 2u; ++i) {
            turbowasm_component_label *other = i == 0u
                ? other_graph.types[i].as.enumeration.labels : other_graph.types[i].as.flags.labels;
            check_true(turbowasm_component_value_type_equal(&graph, indexed(i), &other_graph, indexed(i)));
            other[0] = labels[1]; other[1] = labels[0];
            check_false(turbowasm_component_value_type_equal(&graph, indexed(i), &other_graph, indexed(i)));
        }
        check_true(turbowasm_component_value_type_equal(&graph, indexed(3u), &other_graph, indexed(3u)));
        other_graph.types[2].as.resource.identity = 43u;
        check_false(turbowasm_component_value_type_equal(&graph, indexed(3u), &other_graph, indexed(3u)));
        other_graph.types[2].as.resource.identity = 42u;
        other_graph.types[3].kind = TURBOWASM_COMPONENT_TYPE_BORROW;
        check_false(turbowasm_component_value_type_equal(&graph, indexed(3u), &other_graph, indexed(3u)));
        graph.types[3].kind = TURBOWASM_COMPONENT_TYPE_BORROW;
        check_true(turbowasm_component_value_type_equal(&graph, indexed(3u), &other_graph, indexed(3u)));
        check_false(turbowasm_component_value_type_equal(&graph, indexed(2u), &other_graph, indexed(2u)));
    }
}
