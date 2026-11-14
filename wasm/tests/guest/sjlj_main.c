#include <limits.h>
int sjlj_values(int);
int sjlj_nested(void);
int sjlj_repeated(void);
int sjlj_restore_stack(void);
int sjlj_callback(void);
int control(int unused) { (void)unused; return 42; }
int main(void) {
    const int values[] = {0, 1, 7, -9, INT_MIN, INT_MAX};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (sjlj_values(values[i])) return 1;
    return sjlj_nested() || sjlj_repeated() || sjlj_restore_stack() || sjlj_callback();
}
