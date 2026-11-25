#include "../src/wasi02_component.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

int main(void) {
    const turbowasm_wasi02_interface_desc *tcp =
        turbowasm_wasi02_find_interface("wasi:sockets", "tcp");
    const turbowasm_wasi02_function_desc *start_bind;
    const turbowasm_wasi02_type_desc *address;
    const turbowasm_wasi02_type_desc *ipv6;
    const turbowasm_wasi02_type_desc *port;
    turbowasm_wasi02_value value = {0};
    turbowasm_wasi02_value converted = {0};
    turbowasm_component_value component = {0};
    turbowasm_component_value roundtrip = {0};

    assert(tcp != NULL);
    start_bind = turbowasm_wasi02_find_function(
        tcp, "[method]tcp-socket.start-bind");
    assert(start_bind != NULL);
    assert(start_bind->param_count == 3u);

    address = start_bind->params[2].type;
    assert(address != NULL);
    assert(address->kind == TURBOWASM_WASI02_TYPE_VARIANT);
    assert(address->as.variant.count == 2u);
    ipv6 = address->as.variant.cases[1].payload;
    assert(ipv6 != NULL);
    assert(ipv6->kind == TURBOWASM_WASI02_TYPE_RECORD);
    port = ipv6->as.record.fields[0].type;
    assert(port != NULL);
    assert(port->kind == TURBOWASM_WASI02_TYPE_U16);

    value.kind = TURBOWASM_WASI02_VALUE_U16;
    value.as.u16 = UINT16_MAX;
    assert(turbowasm_wasi02_value_matches_type(port, &value));

    component.kind = TURBOWASM_COMPONENT_TYPE_U16;
    component.as.u16 = UINT16_MAX;
    assert(turbowasm_wasi02_component_value_to_wasi(
               port, &component, &converted) == TURBOWASM_OK);
    assert(converted.kind == TURBOWASM_WASI02_VALUE_U16);
    assert(converted.as.u16 == UINT16_MAX);

    assert(turbowasm_wasi02_component_value_from_wasi(
               port, &converted, &roundtrip) == TURBOWASM_OK);
    assert(roundtrip.kind == TURBOWASM_COMPONENT_TYPE_U16);
    assert(roundtrip.as.u16 == UINT16_MAX);

    turbowasm_component_value_destroy(&roundtrip);
    turbowasm_wasi02_value_destroy(&converted);
    return 0;
}
