#ifndef TURBOWASM_GUEST_CMETA_FIXTURE_H
#define TURBOWASM_GUEST_CMETA_FIXTURE_H
#include <cmeta/struct.h>
Struct(guest_record, (int, value), (double, weight));
const cmeta_struct_desc *guest_peer_metadata(void);
#endif
