/* ============================================================================
 * vsh - Vanguard Shell
 * fuzz_parser.c - Coverage-guided fuzz harness for the lexer -> parser path
 *
 * Feeds arbitrary bytes through the front end (tokenizer + recursive-descent
 * parser) exactly as shell_exec_line() does, but stops before execution -- we
 * never run fuzzer-controlled commands.  The goal is to prove the parser is
 * memory-safe on hostile input: no crashes, no ASan/UBSan reports.
 *
 * Build with libFuzzer:
 *     make fuzz            # produces ./vsh_fuzz
 *     ./vsh_fuzz -max_total_time=90 tests/fuzz_corpus
 *
 * The same file also builds as a standalone corpus driver (feeds stdin or the
 * files named on argv) when compiled with -DFUZZ_STANDALONE, so the harness
 * works even without a libFuzzer-capable toolchain.
 * ============================================================================ */

#include "lexer.h"
#include "parser.h"
#include "arena.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One fuzz iteration: lex then parse `size` bytes of `data`.  Returns 0. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* The lexer treats its input as a C string, so copy into a NUL-terminated
     * buffer (embedded NULs simply terminate the logical input early). */
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    Arena *arena = arena_create();
    if (!arena) { free(input); return 0; }

    Lexer lex;
    lexer_init(&lex, input, arena);
    TokenList *tokens = lexer_tokenize(&lex);

    /* Only parse when the lexer succeeded, mirroring shell_exec_line(). */
    if (tokens && !lex.error) {
        Parser parser;
        parser_init(&parser, tokens, arena);
        ASTNode *ast = parser_parse(&parser);
        (void)ast;   /* Never executed -- we only exercise construction. */
    }

    arena_destroy(arena);
    free(input);
    return 0;
}

#ifdef FUZZ_STANDALONE
/* ---- Standalone driver (no libFuzzer required) -------------------------- *
 * Reads each file named on the command line (or stdin if none) and runs one
 * fuzz iteration over its contents.  Used by the bash-driven random loop when
 * a coverage-guided engine is unavailable. */
#include <stdio.h>

static void run_file(FILE *fp)
{
    char   *buf = NULL;
    size_t  cap = 0, len = 0;
    int     c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 > cap) {
            cap = cap ? cap * 2 : 1024;
            buf = realloc(buf, cap);
            if (!buf) return;
        }
        buf[len++] = (char)c;
    }
    LLVMFuzzerTestOneInput((const uint8_t *)buf, len);
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        run_file(stdin);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        if (!fp) continue;
        run_file(fp);
        fclose(fp);
    }
    return 0;
}
#endif /* FUZZ_STANDALONE */
