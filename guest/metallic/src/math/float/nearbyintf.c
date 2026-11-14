/* See wasm/src/math/double/nearbyint.c for the design rationale.
 *
 * On wasm32, wasm/src/math/float/rintf.c already provides an alias
 * nearbyintf -> rintf. For native builds we add the shim here.
 */
#ifndef __wasm__
float rintf(float);

float nearbyintf(float x)
{
    return rintf(x);
}
#endif
