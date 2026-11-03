/* Native: print parsed events as "t_us HEX..." lines, then "length t_us".
 * Compared against tools/mido_dump.py by test/run_tests.sh. */
#include <stdio.h>
#include <stdlib.h>
#include "smf.h"

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: smf_dump file.mid\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static unsigned char buf[SMF_MAX_INPUT + 1];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    struct smf_song *s;
    char err[128];
    if (smf_parse(buf, n, &s, err, sizeof err)) {
        printf("error %s\n", err);
        return 1;
    }
    for (uint32_t i = 0; i < s->nev; i++) {
        const struct smf_event *e = &s->ev[i];
        printf("%lld", (long long)e->t_us);
        if (e->kind == SMF_EV_SHORT)
            for (uint32_t k = 0; k < e->len; k++)
                printf(" %02X", (e->msg >> (8 * k)) & 0xff);
        else
            for (uint32_t k = 0; k < e->len; k++)
                printf(" %02X", s->sysex[e->msg + k]);
        printf("\n");
    }
    printf("length %lld\n", (long long)s->length_us);
    smf_free(s);
    return 0;
}
