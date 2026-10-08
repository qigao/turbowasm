#include <turbowasm/wasi02_io.h>

static turbowasm_status ready(void *context, bool *out) {
    *out = *static_cast<bool *>(context); return TURBOWASM_OK;
}
static void retain(void *) {}
static void release(void *) {}

int main() {
    turbowasm_wasi02_io domain{};
    turbowasm_wasi02_io_config config{};
    turbowasm_wasi02_io_source source{};
    turbowasm_wasi02_poll_provider poll{};
    turbowasm_wasi02_stream_provider streams{};
    turbowasm_value subscription{};
    bool state = false, result = true;
    turbowasm_wasi02_io_source_ops ops{&state, ready, retain, release};
    turbowasm_wasi02_io_config_init(&config);
    if (turbowasm_wasi02_io_init(&domain, &config, nullptr) != TURBOWASM_OK) return 1;
    if (turbowasm_wasi02_io_source_register(&domain, &ops, &source) != TURBOWASM_OK) return 2;
    if (turbowasm_wasi02_io_pollable_register(&domain, source, &subscription) != TURBOWASM_OK) return 3;
    if (turbowasm_wasi02_io_providers(&domain, &streams, &poll) != TURBOWASM_OK) return 4;
    if (poll.ready(poll.context, subscription, &result) != TURBOWASM_OK || result) return 5;
    state = true;
    if (turbowasm_wasi02_io_source_changed(&domain, source) != TURBOWASM_OK) return 6;
    if (turbowasm_wasi02_io_advance(&domain) != TURBOWASM_OK) return 7;
    if (poll.ready(poll.context, subscription, &result) != TURBOWASM_OK || !result) return 8;
    if (turbowasm_wasi02_io_destroy(&domain) != TURBOWASM_INVALID_ARGUMENT) return 9;
    if (turbowasm_wasi02_io_source_close(&domain, &source) != TURBOWASM_OK) return 10;
    if (poll.drop(poll.context, subscription) != TURBOWASM_OK) return 11;
    return turbowasm_wasi02_io_destroy(&domain) == TURBOWASM_OK ? 0 : 12;
}
