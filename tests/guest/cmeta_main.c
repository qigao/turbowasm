int cmeta_metadata(void);
int cmeta_calls(void);
int cmeta_alignment(void);
int cmeta_object_open(void);
int cmeta_object_read(void);
int cmeta_object_close(void);
int main(void) {
    if (cmeta_metadata() || cmeta_calls() || cmeta_alignment()) return 1;
    if (cmeta_object_open() != 42 || cmeta_object_read() != 42) return 2;
    return cmeta_object_close() == 1 && cmeta_object_close() == 1 ? 0 : 3;
}
