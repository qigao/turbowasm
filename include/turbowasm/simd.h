#ifndef TURBOWASM_SIMD_H
#define TURBOWASM_SIMD_H

#include <turbowasm/status.h>
#include <turbowasm/value.h>

#ifdef __cplusplus
extern "C" {
#endif

turbowasm_status turbowasm_simd_i32x4_add(turbowasm_v128 *out,
                                          const turbowasm_v128 *left,
                                          const turbowasm_v128 *right);
turbowasm_status turbowasm_simd_i32x4_eq(turbowasm_v128 *out_mask,
                                         const turbowasm_v128 *left,
                                         const turbowasm_v128 *right);
turbowasm_status turbowasm_simd_f32x4_mul(turbowasm_v128 *out,
                                          const turbowasm_v128 *left,
                                          const turbowasm_v128 *right);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_SIMD_H */
