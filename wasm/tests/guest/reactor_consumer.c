__attribute__((import_module("provider"), import_name("step"))) int step(int);
int use_provider(int amount) { return step(amount) + 1; }
