/*
 * Phase 1 smoke test: create a terminal, write some bytes, query state.
 * Real example lands in Phase 2 once the parser is wired up.
 */
#include <stdio.h>
#include "spiritty/spiritty.h"

int main(void) {
    sp_options opts;
    sp_options_default(&opts);
    opts.cols = 100;
    opts.rows = 30;

    sp_terminal* t = sp_terminal_create(&opts);
    if (!t) { fprintf(stderr, "sp_terminal_create failed\n"); return 1; }

    sp_terminal_write(t, (const uint8_t*)"hello", 5);
    printf("spiritty %s — %dx%d\n", sp_version(),
           sp_terminal_cols(t), sp_terminal_rows(t));

    sp_terminal_destroy(t);
    return 0;
}
