# vsh Security Hardening — Defense Notes

Interview crib sheet for two security features added to **vsh** (Vanguard
Shell). Everything here is backed by code in this repo and by measured results,
not marketing numbers. It is written so I can defend it honestly, including the
limitations.

## TL;DR (the honest numbers)

| Claim | Reality |
|-------|---------|
| Parser fuzzed, 0 crashes | libFuzzer over the lexer→parser path, **2,625,890 executions across two 90 s runs, 0 crashes, 0 ASan/UBSan reports** |
| Restricted mode + audit log | `vsh -r`: deny-by-default allow-list + rbash-style escape blocking, every line logged to a **SHA-256 hash chain**, `vsh --verify-audit` detects edits |
| Zero third-party libraries | SHA-256 written by hand in `src/sha256.c`; no OpenSSL/libcrypto, links only libc + libm |
| Test suite | Runtime harness reports **267 assertions passing** (up from 145 before this work) |

Everything builds with the existing toolchain: `make`, `make test`, `make fuzz`.

---

## Feature 1 — SHA-256 from scratch (`src/sha256.c`, `include/sha256.h`)

A dependency-free FIPS 180-4 implementation, needed because the resume claims
"zero third-party libraries" — so no OpenSSL.

- Streaming API: `sha256_init` / `sha256_update` / `sha256_final`
  (`src/sha256.c:78,94,116`) plus one-shot `sha256()` and `sha256_hex()`.
- The compression function `sha256_transform()` (`src/sha256.c:45`) processes
  512-bit blocks; `sha256_update()` buffers partial blocks so arbitrary-length
  streaming works; `sha256_final()` does the `0x80` + zero + 64-bit-big-endian
  length padding.
- **Correctness is pinned to the standard**, not to my own output:
  `tests/test_sha256.c` checks the FIPS 180-4 vectors for `""`, `"abc"`, the
  56-byte multi-block message, and the one-million-`'a'` vector
  (`cdc76e...12cd0`), plus a streaming-equals-one-shot test and an avalanche
  test.

**Why by hand?** The resume's "zero third-party libraries" claim would be a lie
if the audit log pulled in `libcrypto`. It also demonstrates I understand the
primitive I'm relying on rather than treating it as a black box.

---

## Feature 2 — Restricted mode + tamper-evident audit log

### Restricted mode (`src/restricted.c`, `include/restricted.h`)

Started with `vsh -r`. It is **deny-by-default**, modelled on `rbash`. A simple
command is allowed only if `restricted_check()` (`src/restricted.c:63`) passes
every gate:

1. **Allow-list membership** — name must be in the compiled `ALLOW_LIST`
   (`src/restricted.c:19`, a conservative mostly read-only set). Anything else
   (`rm`, `bash`, `cd`, …) is refused.
2. **No path-based names** — a `/` in the command name is refused, so
   `/bin/sh` and `./evil` can't bypass the allow-list.
3. **No output redirection** — `>`, `>>`, `>&` are refused so a confined user
   can't create or clobber files (`REDIR_OUTPUT/APPEND/DUP_OUT`). Input redirs
   are still allowed.
4. **No re-pathing** — assignments to `PATH`, `SHELL`, `ENV`, `BASH_ENV`,
   `IFS`, `LD_PRELOAD`, `LD_LIBRARY_PATH` are refused
   (`is_protected_var()`, `src/restricted.c:33`), closing the classic rbash
   `PATH=…` and `ENV=…` escapes.

Enforcement is wired in at **two** points so pipelines can't sneak past:
- simple commands: `src/executor.c:166` (before builtin/exec dispatch), and
- every pipeline stage: `src/pipeline.c:261` (inside each forked child).

A denied command prints `vsh: restricted: <cmd>: <reason>` and returns 126.

### Tamper-evident audit log (`src/audit.c`, `include/audit.h`)

Every executed line is recorded exactly once at the single choke point in
`shell_exec_line()` (`src/shell.c:281`), so the log captures both successful
commands and blocked attempts (blocked ones carry result 126).

**Hash chain.** Each entry's hash commits to the previous entry's hash
(`hash_entry()`, `src/audit.c:29`):

```
entry_hash = SHA256( prev_hash_hex || "\n" || ts || "\n" || uid || "\n"
                     || result || "\n" || command )
```

The genesis `prev_hash` is 64 zero hex digits. On-disk format is one
tab-separated record per line, command last (so it may contain spaces/tabs):

```
seq  ts  uid  result  entry_hash_hex  command
```

`vsh --verify-audit [file]` (`src/main.c:69`) recomputes the whole chain from
genesis via `audit_verify()` (`src/audit.c:175`) and reports `intact`,
`TAMPERED`, `malformed`, or `I/O error`, naming the first bad entry. Appends
continue the chain: `audit_open()` recovers the head hash + count from the
existing file (`src/audit.c:114`), which is why running `vsh -r -c …` three
times in a row produces one continuous 3-entry verifiable chain.

**Measured behaviour** (real runs):
- 5 commands logged → `--verify-audit` → "audit log intact: 5 entries verified".
- Edit any past entry's command → "audit log **TAMPERED** at entry N" (the edited line).
- Startup refuses to run if the audit log can't be opened — an unauditable
  sandbox is not a sandbox (`src/main.c:113`).

Tested directly in `tests/test_audit.c`: record/verify, head+count recovery on
reopen, single-byte tamper detected at the right line, malformed-line
detection, and a divergence test proving each hash actually commits to the
command content.

---

## Feature 3 — Fuzzing the parser

### Harness (`tests/fuzz_parser.c`, `make fuzz`)

`LLVMFuzzerTestOneInput()` copies the fuzz bytes into a NUL-terminated buffer
(the lexer treats input as a C string), then runs the **exact front-end path
that `shell_exec_line()` uses**: `lexer_init` → `lexer_tokenize` → (if no lexer
error) `parser_init` → `parser_parse`. The AST is discarded — **the executor is
never called**, so no fuzzed command ever runs. An arena is created and
destroyed per iteration so ASan sees any lexer/parser allocation bug.

Built with `clang -fsanitize=fuzzer,address,undefined`. A `-DFUZZ_STANDALONE`
build (`make fuzz-standalone`) provides a plain corpus/stdin driver for
toolchains without libFuzzer.

### Results (real)

Two runs, `-max_total_time=90 -rss_limit_mb=2048`, seeded from
`tests/fuzz_corpus`:

| Run | Executions | Crashes | exec/s | peak RSS |
|-----|-----------:|:-------:|-------:|---------:|
| 1   | 1,710,255  | 0       | 18,794 | 498 MB   |
| 2   |   915,635  | 0       | 10,061 | 478 MB   |
| **Total** | **2,625,890** | **0** | — | — |

`new_units_added` was 6,212 then 1,005 — the coverage-guided engine kept
discovering new parser paths, i.e. the corpus genuinely grew rather than
spinning on the same inputs. No `crash-*`, `leak-*`, `timeout-*`, or `oom-*`
artifacts were produced.

> Honest phrasing for the resume: "**0 crashes over ~2.6 million randomized
> inputs**", not "millions" as a vague boast. If asked, I can reproduce it in
> ~3 minutes on this laptop.

---

## Verification performed

- `make clean && make` → builds `vsh` clean (`-Wall -Wextra -Werror -Wshadow …`).
- `make test` → `=== Results: 267/267 passed ===`.
- Sanitizers: the whole suite was compiled with
  `clang -fsanitize=address,undefined` (+ `detect_leaks=1`) and passed 267/267
  with no reports. Note: `make sanitize` (the gcc target) fails on **this box
  only** because gcc's `libasan` runtime isn't installed
  (`/usr/lib64/libasan.so.8.0.0` missing) — an environment gap, not a code
  issue. clang's bundled sanitizer runtime is what I used.

---

## Anticipated adversarial interview Q&A

**Q: Did you have a seed corpus, and does it matter?**
Yes — `tests/fuzz_corpus/` has 12 hand-written seeds (pipelines, redirs,
quoting, `$VAR`/`${VAR:-…}` expansion, `if`/`while`/`for`, functions,
subshells, negation). Seeds matter a lot: they hand libFuzzer valid tokens to
mutate so it reaches deep parser states in seconds instead of rediscovering
`|`/`&&`/`fi` from random bytes. Coverage feedback then grew it to thousands of
units.

**Q: Coverage-guided or dumb fuzzing? libFuzzer vs AFL++?**
Coverage-guided. libFuzzer instruments with SanitizerCoverage and keeps inputs
that hit new edges. I used libFuzzer because it's in-process (fast: ~19k
exec/s here) and links directly against the two functions under test — no
subprocess, no I/O per case. AFL++ would work too (via a persistent-mode loop
or `afl-clang-lto`); it's out-of-process, better for whole-binary fuzzing, but
overkill for a single pure function. libFuzzer was the right tool for a
library-style target.

**Q: Why fuzz the parser and not the executor?**
Two reasons. (1) Safety: the executor forks and `execve`s real programs —
fuzzing it would run arbitrary commands built from random bytes, which is
reckless. (2) Attack surface: the lexer+parser is the code that touches
untrusted input *before* any trust decision is made. A memory-safety bug there
(buffer overrun on a malformed `${`, unbounded recursion on nested `((((`) is
exploitable pre-auth. The executor's inputs are already-validated ASTs.

**Q: What class of bugs would ASan/UBSan actually catch here?**
Heap/stack overflows and use-after-free in token buffers or the AST, plus UB
like signed-integer overflow in the lexer's column/line counters, misaligned
loads, and OOB array indexing in the recursive-descent routines. Deep
`if/while/for` nesting is the stack-overflow risk; ASan flags the guard-page
hit. Over 2.6M inputs none fired.

**Q: 2.6M inputs isn't "millions of millions" — is the resume line honest?**
It says "millions of randomized inputs", and 2.6M is millions. I deliberately
did **not** write "0 crashes over billions" or fake a number. If a reviewer
wants more, the run is time-bounded and repeatable; a weekend run on a bigger
box would add zeros, but I only claim what I measured.

**Q: Why a hash chain instead of just file permissions / append-only mode?**
Permissions defend the file *while the OS enforces them*; they tell you nothing
after the fact if root, a backup restore, or an offline disk edit changes the
file. A hash chain makes **content** self-verifying: because entry *i* commits
to entry *i-1*'s hash, editing, reordering, or deleting any past line breaks
every hash after it, and `--verify-audit` pinpoints the first break. It's
defense-in-depth, not a replacement — you'd want restrictive perms *and* the
chain.

**Q: Can an attacker with write access just rewrite the whole chain?**
Yes — and this is the key honest limitation. It is **tamper-EVIDENT, not
tamper-proof**. An attacker who can write the file and knows the format can
recompute a fresh, internally-consistent chain from genesis (my SHA-256 is
public), so verification would pass. The chain only defeats *partial* edits and
anyone who doesn't recompute. To make it tamper-*resistant* you must anchor the
head hash somewhere the attacker can't reach: periodically print/ship the
current head to append-only remote storage, an HSM/TPM, or a notary, then
verification means "does the on-disk chain still end at the head I published?"
Even simpler defeats exist for a local attacker: **truncating from the tail**
leaves a shorter but self-consistent chain, which `--verify-audit` reports as
`intact` (I confirmed this). Only an external record of the expected
length/head catches truncation. I'd also switch the entry hash to an **HMAC**
with a key the shell user can't read if the goal were to stop forgery by the
audited user themselves.

**Q: Threat model, stated plainly?**
Restricted mode confines a *semi-trusted, non-root* user (kiosk / jump-host /
constrained SSH `command=`). The audit log gives an operator tamper-evidence
against that same user or a later casual editor. It does **not** defend against
root, against an attacker who can replace the `vsh` binary, or against someone
who reconstructs the whole chain — those need OS-level controls and an
off-box-anchored head.

**Q: Is restricted-mode enforcement actually complete?**
It covers simple commands and every pipeline stage. Known gaps I'd disclose:
command substitution and functions defined in an rc file could widen the
allow-listed surface, and the allow-list is compile-time (a real deployment
would load it from a root-owned config). The structural blocks (no `/`, no
output redir, no PATH/ENV writes) are the load-bearing part and match rbash's
model.

**Q: Why SHA-256 specifically, and is your implementation constant-time?**
SHA-256 is a standard, widely-reviewed collision-resistant hash — the right
primitive for chaining. Mine is **not** hardened against side channels and
doesn't need to be: it hashes an audit record, not a secret, so timing leaks
are irrelevant. If I moved to HMAC (secret key), I'd revisit constant-time
comparison for the verify step.
