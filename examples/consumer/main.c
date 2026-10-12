/* The consumer program: it creates a live engine through the public header and releases it. That is the whole
 * composition root of a C consumer, and the one place a live value is named (AGENTS.md, "Inject, never reach"). */
#include <bc_file_system/bc_file_system.h>

#include <stdio.h>

int main(void) {
    bc_file_system_engine *engine = NULL;
    bc_file_system_error error;
    const int32_t status = bc_file_system_engine_create_live(&engine, &error);
    if (status != 0) {
        fprintf(stderr, "engine_create_live failed: kind %d, system code %d\n", (int)error.kind,
                (int)error.system_code);
        return 1;
    }
    bc_file_system_engine_release(engine);
    puts("bc-file-system: a live engine was created and released");
    return 0;
}
