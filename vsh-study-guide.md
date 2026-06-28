# vsh — Complete Beginner's Study Guide
*Jacob Antony Jeejo · Airspan interview prep · Jun 2026*

---

## HOW TO USE THIS GUIDE

You built vsh using Claude, so you've never read through it or understood it yourself. This guide teaches you the project from scratch, assuming only basic C knowledge. By the end, you should be able to explain every module clearly to an Airspan interviewer.

**Read order:** Part 0 → Part 1 → Part 2 → then pick modules in Part 3 by study day.

---

## PART 0: What Is vsh and Why Does It Exist?

### You already use a shell every day

When you open a terminal and type `ls -la`, something reads that text, finds the `ls` program on your system, runs it, and shows you the output. That "something" is a **shell**.

Shells you know: **bash** (default on most Linux), **zsh** (macOS default), **fish**. They all do the same basic thing.

**vsh is a shell — the same kind of program — written by you from scratch in C.**

### What does it look like from the outside?

```
jake@laptop:~$ ls -la | grep .txt && echo "found some"
-rw-r--r-- 1 jake jake 1234 Jun 5 notes.txt
found some
jake@laptop:~$ _
```

Everything happening here — reading your keystrokes, running `ls`, routing its output into `grep`, running `echo` only if grep succeeded — is vsh doing its job.

### What happens internally when you type `ls -la | grep .txt`?

There are exactly four steps, handled by four modules:

```
Step 1: vsh_readline.c reads your keystrokes
        → returns the string: "ls -la | grep .txt"

Step 2: lexer.c breaks the string into pieces
        → [ls] [-la] [|] [grep] [.txt]
           each piece is called a TOKEN

Step 3: parser.c understands the structure
        → PIPELINE: { run ls -la, pipe its output into grep .txt }

Step 4: executor.c + pipeline.c actually run the programs
        → fork ls, fork grep, connect them with a pipe, wait for both
```

That's the whole shell. Every feature in vsh maps to one of these four steps.

### What makes vsh impressive (and interview-worthy)?

Most people who implement a shell use:
- `readline` library (handles the line editing)
- Lots of individual `malloc/free` calls everywhere

vsh does neither:

1. **Zero external dependencies** — not even `readline`. The line editor is 950 lines of custom raw terminal I/O.
2. **Arena allocator** — instead of individual `malloc/free` for every token and AST node, vsh pre-allocates a memory region and hands out slices. One `arena_reset()` call frees everything.
3. **Real Unix job control** — Ctrl+Z, `fg`, `bg`, background jobs (`sleep 100 &`) all implemented from scratch using Unix process groups and signals.

These are exactly the C and systems skills Airspan wants.

---

## PART 1: C Concepts You Need Before Reading the Code

You know C++ with pointers. C is simpler but more explicit. Here's what you need.

### 1.1 Strings in C (No std::string Here)

In C++, `std::string` is a class — it manages its own memory, knows its length, handles copies safely.

In C, **there is no string type.** A "string" is just an array of `char` that ends with a special character: `'\0'` (the null terminator, ASCII value 0).

```c
// C++ way
std::string name = "Jacob";
name.length();       // member function, easy

// C way
char *name = "Jacob";   // name is a POINTER to the first character 'J'
strlen(name);           // function that counts chars until it hits '\0'
```

"Jacob" in memory actually looks like this:
```
Address: 1000  1001  1002  1003  1004  1005
Content:  'J'   'a'   'c'   'o'   'b'  '\0'
```

`char *name = "Jacob"` means `name` holds the address `1000`. The `'\0'` at address 1005 is how `strlen`, `printf`, and all string functions know where the string ends.

**Key danger:** if you write past the end of a buffer (the classic buffer overflow), you overwrite whatever memory comes after it — neighboring variables, return addresses, anything. This is why `SafeString` exists in vsh.

### 1.2 malloc and free (C's new and delete)

In C++: `new Type` allocates on heap, `delete ptr` frees it.
In C: `malloc(bytes)` allocates heap memory, `free(ptr)` releases it.

```c
// Allocate space for 100 characters on the heap
char *buffer = malloc(100);
if (buffer == NULL) {
    // malloc failed (system out of memory — rare but must be checked)
    return -1;
}
// ... use buffer ...
free(buffer);   // MUST call this, or you have a memory leak
```

`malloc` returns `void *` — a pointer to "unknown type". In C (unlike C++), you can assign a `void *` to any pointer type without a cast:

```c
char *p = malloc(100);   // fine in C, needs (char*) cast in C++
```

**Memory leak:** forgetting to call `free()`. In a shell that runs for hours, even small leaks accumulate and eventually crash the process.

### 1.3 Structs — Grouping Data Without Methods

C doesn't have classes. A `struct` groups related data:

```c
// Define the type
typedef struct {
    int  type;     // what kind of token (WORD, PIPE, etc.)
    char *value;   // the actual string ("ls", "|", etc.)
    int  line;     // which line of input
    int  col;      // which column
} Token;

// Use it
Token t;
t.type  = 5;
t.value = "ls";
t.line  = 1;
t.col   = 0;
```

vsh is built entirely out of structs: `Token`, `ASTNode`, `Shell`, `Arena`, `SafeString`, `Job`.

### 1.4 Pointers to Structs and the -> Operator

When you pass a struct to a function, C copies the whole thing. For large structs, that's slow and the function can't modify the original.

The solution: pass a *pointer* to the struct.

```c
// BAD: copies the entire Shell struct (could be kilobytes)
void run_command(Shell shell) { ... }

// GOOD: passes 8 bytes (the size of a pointer)
void run_command(Shell *shell) { ... }
```

Inside the function, access members with `->`:

```c
// These two are identical:
shell->last_status = 0;    // shorthand, more common
(*shell).last_status = 0;  // what it actually means
```

This is why **every function in vsh takes `Shell *shell`** — they all share the same shell state through one pointer.

### 1.5 File Descriptors — Linux's I/O Numbers

In Linux, every open I/O channel gets a small integer called a **file descriptor (fd)**:

- **0 = stdin** — keyboard input (by default)
- **1 = stdout** — terminal output (by default)
- **2 = stderr** — error output (by default)
- **3, 4, 5...** — files you open yourself

When you `open("notes.txt", O_RDONLY)`, the kernel returns the next available number (e.g., 3).

```c
// Both write "hello" to stdout, but:
printf("hello\n");         // C library function — buffers, uses fd 1 internally
write(1, "hello\n", 6);   // direct Linux system call — unbuffered, immediate
```

vsh's line editor uses `write()` directly instead of `printf()` because `write()` is safe to call inside signal handlers (more on this later). `printf()` is not.

### 1.6 fork() and exec() — The Unix Way to Run Programs

This is the most important concept in vsh. When you type `ls`, the shell doesn't call a function named `ls` — it creates a new process.

**fork()** creates an exact copy of the current process:

```c
pid_t pid = fork();
// After fork(), two identical processes are running

if (pid == 0) {
    // This is the CHILD process (fork returned 0 to the child)
} else if (pid > 0) {
    // This is the PARENT process (fork returned the child's PID)
} else {
    // fork() failed (pid == -1)
}
```

**execvp()** replaces the current process with a new program:

```c
char *args[] = { "ls", "-la", NULL };  // NULL-terminated array
execvp("ls", args);
// If we reach this line, exec failed (ls not found, etc.)
```

Together, they run a program without destroying the shell:

```
Shell (PID 100) calls fork()
│
├─► Parent (PID 100): calls waitpid(101) — blocks until child exits
│
└─► Child  (PID 101): calls execvp("ls", args)
                      → child is REPLACED by ls
                      → ls runs, prints files, exits
                      → PID 101 dies
                      → parent's waitpid() returns
                      → shell shows next prompt
```

### 1.7 Pipes — Connecting Programs

A **pipe** is a one-directional in-memory buffer with two ends:

```c
int fds[2];
pipe(fds);
// fds[0] = read end  (data comes OUT here)
// fds[1] = write end (data goes IN here)
```

For `ls | grep .txt`, you want ls's stdout to connect to grep's stdin. You do this with `dup2()`:

```c
// In the ls child:
dup2(fds[1], 1);   // make fd 1 (stdout) point to the pipe's write end
// Now ls's output goes into the pipe

// In the grep child:
dup2(fds[0], 0);   // make fd 0 (stdin) point to the pipe's read end
// Now grep reads ls's output from the pipe
```

`dup2(old_fd, new_fd)` makes `new_fd` refer to the same file as `old_fd`. If `new_fd` was already open (like fd 1 = stdout), it's closed first.

---

## PART 2: The Architecture — What Connects to What

### The Simple Picture

```
 You type a command
        │
        ▼
┌─────────────────────────────────────────────────────┐
│  vsh_readline.c  ─  The Line Editor                 │
│  Reads keystrokes one by one in raw mode            │
│  Handles: ↑↓ history, Tab completion, ←→ cursor    │
│  Returns: char *line  (e.g., "ls -la | grep .txt")  │
└────────────────────────┬────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│  lexer.c  ─  The Tokenizer                          │
│  Breaks the string into named pieces (tokens)       │
│  Returns: TokenList containing e.g.:                │
│    WORD:"ls"  WORD:"-la"  PIPE  WORD:"grep"  EOF   │
└────────────────────────┬────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│  parser.c  ─  The Syntax Analyzer                   │
│  Figures out the structure of the command           │
│  Returns: ASTNode * (a tree, e.g., PIPELINE node)  │
└────────────────────────┬────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│  executor.c  ─  The Command Runner                  │
│  Walks the tree and runs everything                 │
│  ├── pipeline.c    (fork + pipe + exec multiple)   │
│  ├── job_control.c (process groups, fg/bg)          │
│  └── builtins.c    (cd, export, exit, ...)          │
└─────────────────────────────────────────────────────┘

Support libraries (used by all modules above):
┌──────────────────┐   ┌──────────────────────────────┐
│  arena.c         │   │  safe_string.c               │
│  Fast allocator  │   │  Bounds-checked string builder│
│  for tokens/AST  │   │  used by lexer + readline    │
└──────────────────┘   └──────────────────────────────┘
```

### The Data Flow for One Command

For `ls -la | grep .txt`:

```
1. vsh_readline returns: char *line = "ls -la | grep .txt"
2. arena_reset() clears the parse arena (previous command's tokens/AST gone)
3. lexer_tokenize() → TokenList: [WORD:"ls"] [-la] [PIPE] [WORD:"grep"] [".txt"] [EOF]
4. parser_parse()   → ASTNode*: PIPELINE { cmd1: ["ls","-la"], cmd2: ["grep",".txt"] }
5. executor dispatches to pipeline.c
6. pipeline.c creates 1 pipe, forks 2 children, wires them, waits
7. Both programs run; grep's output goes to terminal
8. arena_reset() frees all tokens and AST nodes in O(1)
```

---

## PART 3: Module Deep-Dives

---

### MODULE 1 — arena.c (The Memory Manager)

**File:** [src/arena.c](src/arena.c)

#### What Problem Does It Solve?

The parser builds a tree of structs (called AST nodes) for every command. After the command executes, all those nodes are useless — they should be freed.

If you used normal `malloc/free`, you'd need to call `free()` on every single node. For a complex command like `if [ $x -gt 0 ]; then ls; elif ...; fi`, that's potentially dozens of individual frees. Forget one = memory leak. Free one twice = crash.

The arena solves this: **allocate from a pool, reset the whole pool at once.**

#### The Analogy

Think of shopping with a basket (arena) vs. carrying each item in your hands (individual malloc). Grabbing an item from the basket is instant (no searching for space). When you're done shopping, you dump the whole basket at once — you don't put each item back individually.

#### The Data Structures

```c
// A "page" is one large chunk of memory
typedef struct ArenaPage {
    struct ArenaPage *next;  // linked list → if this page fills up, get another
    size_t size;             // total bytes in this page
    size_t used;             // how many bytes have been given out
    char data[];             // the actual memory (C99 "flexible array member")
} ArenaPage;

typedef struct {
    ArenaPage *head;         // first page (never freed until arena_destroy)
    ArenaPage *current;      // page we're currently allocating from
    size_t page_size;        // default size for new pages (usually 4096 bytes)
} Arena;
```

**What is `char data[]`?** This is a C99 feature called a "flexible array member." The struct declaration has no size for `data`. When you `malloc(sizeof(ArenaPage) + 4096)`, the 4096 bytes are immediately after the struct in memory — `data` points right to them. No second malloc needed.

#### How Allocation Works

```
Page state before allocating 32 bytes:
┌──────────────────────────────────────────────────────┐
│ used=200 │←── already given out ──→│←── free space ──│
│          │          200 bytes      │    3896 bytes   │
└──────────────────────────────────────────────────────┘
                                     ↑
                          next allocation starts here

After allocating 32 bytes:
┌──────────────────────────────────────────────────────┐
│ used=232 │←───── already given out ─────→│←── free ──│
│          │            232 bytes          │  3864 bytes│
└──────────────────────────────────────────────────────┘
```

This is called **bump allocation** — just bump the `used` counter forward by the requested size. No searching for free space. No bookkeeping. Always O(1).

The actual code:

```c
void *arena_alloc(Arena *arena, size_t size) {
    size_t aligned = align_up(size);           // round up to multiple of 8
    ArenaPage *page = arena->current;

    if (page->used + aligned <= page->size) {
        void *ptr = page->data + page->used;   // pointer to free space
        page->used += aligned;                 // bump the counter
        return ptr;
    }
    // If page is full: allocate a new page, link it, allocate there
    ...
}
```

#### The align_up() Trick

```c
static inline size_t align_up(size_t size) {
    return (size + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);
}
// ARENA_ALIGNMENT is 8
```

CPUs are faster when data starts at addresses divisible by 8 (called "aligned"). If you allocate 13 bytes, the next allocation should start at address 16 (not 13) to stay aligned.

`align_up(13)` step by step:
- `ARENA_ALIGNMENT - 1` = `7` = `0000...0111` in binary
- `13 + 7` = `20` = `0001 0100`
- `~7` = `...1111 1000` (all bits set except lowest 3)
- `20 & ~7` = `0001 0000` = `16` ✓

The mask `~(ARENA_ALIGNMENT-1)` zeros out the lowest 3 bits, which rounds down to a multiple of 8. Adding 7 first ensures we round *up*, not down.

#### How arena_reset Works

```c
void arena_reset(Arena *arena) {
    // Free every page AFTER the first
    ArenaPage *page = arena->head->next;
    while (page) {
        ArenaPage *next = page->next;
        free(page);
        page = next;
    }
    // Reset the first page's counter (the memory is still allocated)
    arena->head->next = NULL;
    arena->head->used = 0;
    arena->current = arena->head;
}
```

The first page is *not* freed — its memory is kept for the next command. Only extra pages (allocated if the command needed more than 4096 bytes) are freed. The `used` counter is set to 0, so the first page's memory is treated as empty again.

One call. Entire command's tokens and AST nodes gone. O(1) regardless of how many allocations were made.

#### Interview Answer

> "An arena allocator pre-allocates a large memory page and hands out slices by bumping a pointer. Individual allocations are O(1) with no bookkeeping. Reset is also O(1) — reset the page counter, free any overflow pages. In vsh, every token and AST node is arena-allocated. After a command executes, one `arena_reset()` call frees the entire parse state. This eliminates the possibility of leaking parser memory. The same pattern exists in embedded systems — FreeRTOS memory pools and Zephyr's `k_mem_pool` work on the same idea."

---

### MODULE 2 — safe_string.c (The String Builder)

**File:** [src/safe_string.c](src/safe_string.c)

#### What Problem Does It Solve?

The lexer reads input character by character and builds up token strings. It doesn't know in advance how long `"my very long argument with spaces"` will be. It needs a string that can grow.

In C, you can't just append to a `char *` — you'd need to know the buffer size and stop before overflowing it. `SafeString` is a dynamically-growing string with automatic bounds checking.

#### The Data Structure

```c
typedef struct {
    char   *data;   // pointer to the character buffer
    size_t  len;    // current string length (not counting '\0')
    size_t  cap;    // total buffer capacity (how much data[] can hold)
} SafeString;
```

Think of it like this:
```
SafeString s:
  data → [ 'l' 's' '-' 'l' 'a' '\0' ??? ??? ??? ??? ]
  len  = 5
  cap  = 10
```
`len=5` means we've written 5 chars. `cap=10` means there's room for 10 total. Room for 4 more characters before we need to grow.

#### The 2x Growth Strategy

When you try to append and there's not enough room:

```c
static bool sstr_ensure(SafeString *s, size_t needed) {
    if (needed <= s->cap) return true;  // enough room, no action

    size_t new_cap = s->cap * 2;        // DOUBLE the capacity
    if (new_cap < needed) new_cap = needed;

    char *new_data = realloc(s->data, new_cap);
    if (!new_data) return false;        // allocation failed

    s->data = new_data;
    s->cap  = new_cap;
    return true;
}
```

**Why double?**

If you grew by 1 byte each time, appending N characters would trigger N reallocations — each copying the existing content. Total work: 1 + 2 + 3 + ... + N = O(N²). Terrible.

Doubling means at most log₂(N) reallocations for N appends. Each append is O(1) *amortized* (averaged over many appends). This is the same strategy `std::string` and `std::vector` use internally in C++.

#### The Double-Pass vsnprintf Pattern

```c
bool sstr_appendf(SafeString *s, const char *fmt, ...) {
    va_list ap;

    // PASS 1: Measure how many bytes the formatted string will need
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);  // NULL buffer → just measure
    va_end(ap);

    if (!sstr_ensure(s, s->len + needed + 1)) return false;

    // PASS 2: Actually write the formatted string into the buffer
    va_start(ap, fmt);
    vsnprintf(s->data + s->len, needed + 1, fmt, ap);
    va_end(ap);

    s->len += needed;
    return true;
}
```

`vsnprintf(NULL, 0, fmt, ap)` is a POSIX trick: when the buffer is NULL and size is 0, it doesn't write anything — it just returns how many bytes *would* be written. This lets you measure before allocating, making overflow impossible.

#### Key C Concept — va_list

The `...` in `sstr_appendf(SafeString *s, const char *fmt, ...)` means "accepts any number of extra arguments" (like `printf`). `va_list` and the macros `va_start`, `va_end` let you access those extra arguments. You can't reuse a `va_list` after reading it — that's why there are two separate `va_start`/`va_end` blocks, one for each pass.

#### Interview Answer

> "SafeString is a dynamically-growing string buffer with automatic bounds checking. It uses 2× capacity growth to give amortized O(1) appends — the same algorithm `std::string` uses. For formatted strings, I use the double-pass vsnprintf pattern: first call with a NULL buffer to measure the required size, then allocate, then write — making format string overflow impossible. In embedded contexts, you'd typically use a fixed ring buffer instead, but the growth pattern is useful wherever memory is abundant."

---

### MODULE 3 — lexer.c (The Tokenizer)

**File:** [src/lexer.c](src/lexer.c)

#### What Is a Lexer?

A **lexer** (also: tokenizer, scanner) takes a raw string and produces a list of **tokens** — the meaningful units of the language, each tagged with its type.

Think of it like reading a sentence: "the quick fox" → [Article: "the"] [Adjective: "quick"] [Noun: "fox"]. The lexer doesn't know what these words *mean* together — that's the parser's job. It just identifies and classifies each piece.

**Why not just split on spaces?**

```bash
echo "hello world"   → one argument: hello world   (space inside quotes = literal)
ls -la               → two tokens: ls and -la
grep foo|bar         → three tokens: grep, |, bar   (no spaces around |)
```

Space-splitting fails for all of these. You need a state machine that understands quoting, escaping, and multi-character operators.

#### Token Types in vsh

```
TOK_WORD         →  "ls", "-la", "file.txt", "/home/jake"
TOK_PIPE         →  |
TOK_AND          →  &&
TOK_OR           →  ||
TOK_SEMICOLON    →  ;
TOK_AMPERSAND    →  &     (background: sleep 100 &)
TOK_REDIRECT_IN  →  <
TOK_REDIRECT_OUT →  >
TOK_APPEND       →  >>
TOK_IF, TOK_THEN, TOK_ELSE, TOK_FI    (shell keywords)
TOK_WHILE, TOK_DO, TOK_DONE
TOK_FOR, TOK_IN
TOK_EOF          →  end of input
```

#### How the Lexer Works

The lexer has a position (`pos`) in the input string. It reads one character at a time, decides what kind of token it's seeing, and builds the token.

For `ls -la | grep .txt`:

```
pos=0: 'l' — start of a word. Collect until whitespace or operator.
             → build: 'l', 's' → WORD "ls"
pos=2: ' ' — whitespace, skip
pos=3: '-' — start of a word ('-' alone is not an operator here)
             → build: '-', 'l', 'a' → WORD "-la"
pos=6: ' ' — skip
pos=7: '|' — peek next char: ' ' (not '|')
             → PIPE token (single pipe, not ||)
pos=8: ' ' — skip
pos=9: 'g' — start of word → WORD "grep"
pos=13:' ' — skip
pos=14:'.' — start of word → WORD ".txt"
pos=18: end → TOK_EOF
```

#### Greedy Matching

Some operators are 1 or 2 characters: `>` vs `>>`, `|` vs `||`, `&` vs `&&`. The lexer uses **greedy matching**: when it sees `>`, it immediately peeks at the next character. If it's also `>`, consume both and emit `>>`. Otherwise emit `>`.

```c
// Simplified from lexer.c
case '>':
    if (lex_peek(lex, 1) == '>') {
        lex_advance(lex);  // consume both '>' characters
        lex_advance(lex);
        return make_token(TOK_APPEND, ">>", line, col);
    } else {
        lex_advance(lex);
        return make_token(TOK_REDIRECT_OUT, ">", line, col);
    }
```

The || check happens before | — always check the longer option first.

#### Quoting

```bash
echo "hello world"    → space inside "" is literal, not a token separator
echo 'don'"'"'t'      → quoting gymnastics for an apostrophe
echo hello\ world     → backslash escapes the space
```

The lexer tracks a quoting mode. When it enters `"..."`, spaces and operators are treated as literal characters and added to the current word. When it enters `'...'`, *everything* including `$` is literal. The word being built is stored in a `SafeString` that grows as characters are added.

#### Where arena Fits

Every token's `value` string (the `char *value` field) is allocated with `arena_strdup(lex->arena, ...)`. The token list itself (`TokenList`) is also arena-allocated. When `arena_reset()` is called after the command executes, all token strings disappear in one shot.

#### Interview Answer

> "The lexer is a hand-written finite state machine. It walks the input character by character, tracking quoting state (unquoted, single-quoted, double-quoted), and builds tokens using a SafeString that grows as needed. Greedy matching handles multi-character operators by checking the longer form first. All token strings are arena-allocated so cleanup is O(1). In embedded systems, this exact FSM pattern appears in AT command parsers, NMEA GPS parsers, and any protocol frame decoder."

---

### MODULE 4 — parser.c (The Syntax Analyzer)

**File:** [src/parser.c](src/parser.c)

#### What Is a Parser?

The lexer gave us a flat list of tokens: `[ls] [-la] [|] [grep] [.txt]`. A flat list doesn't capture *structure*. Consider:

```bash
ls && echo ok || echo fail
```

Tokens: `[ls] [&&] [echo] [ok] [||] [echo] [fail]`

How do we interpret this?
- `(ls && echo ok) || echo fail` — if ls+echo-ok fails, run echo fail
- `ls && (echo ok || echo fail)` — ls must succeed; then echo ok or fail

Just like arithmetic `2 + 3 * 4 = 14` (not 20) because `*` has higher precedence than `+`, shell operators have precedence: `|` binds tighter than `&&`, which binds tighter than `||`, which binds tighter than `;`.

A **parser** uses grammar rules to build a tree that encodes the correct precedence.

#### The Grammar (Simplified)

```
program  → list EOF
list     → pipeline ( ('&&' | '||' | ';' | '&') pipeline )*
pipeline → command ('|' command)*
command  → simple_command
         | if_stmt
         | while_stmt
         | for_stmt
         | function_def
         | subshell
```

Reading this: "A program is a list. A list is one or more pipelines connected by &&/||/;/&. A pipeline is one or more commands connected by |."

#### Recursive Descent — One Function Per Rule

vsh uses **recursive descent parsing**: each grammar rule becomes one C function. The functions call each other in a way that naturally handles precedence.

```c
// parse_program calls parse_list
// parse_list calls parse_pipeline
// parse_pipeline calls parse_command

// parse_list handles && and ||
static ASTNode *parse_list(Parser *p) {
    ASTNode *left = parse_pipeline(p);  // parse the left side first

    while (check(p, TOK_AND) || check(p, TOK_OR)) {
        TokenType op = cur_token(p)->type;
        advance(p);                         // consume the && or ||
        ASTNode *right = parse_pipeline(p); // parse the right side

        ASTNode *node = arena_calloc(p->arena, 1, sizeof(ASTNode));
        node->type  = (op == TOK_AND) ? NODE_AND : NODE_OR;
        node->left  = left;
        node->right = right;
        left = node;  // this AND/OR node becomes the new left for the next iteration
    }
    return left;
}
```

Because `parse_list` calls `parse_pipeline`, which calls `parse_command`, the tree structure ensures that `|` binds tighter than `&&` — exactly right.

#### The AST (Abstract Syntax Tree)

For `ls | grep .txt && echo done`:

```
         AND
        /    \
  PIPELINE    COMMAND
  /      \      args: ["echo", "done"]
CMD      CMD
["ls"]  ["grep", ".txt"]
```

This tree is made of `ASTNode` structs:

```c
typedef struct ASTNode {
    NodeType type;           // NODE_COMMAND, NODE_PIPELINE, NODE_AND, ...

    // Used for NODE_COMMAND:
    char **args;             // argv array: ["ls", "-la", NULL]
    int    argc;
    Redirect *redirs;        // I/O redirections for this command

    // Used for NODE_PIPELINE, NODE_AND, NODE_OR, NODE_SEQUENCE:
    struct ASTNode *left;
    struct ASTNode *right;

    // Used for NODE_IF:
    struct ASTNode *condition;
    struct ASTNode *then_branch;
    struct ASTNode *else_branch;

    // Used for NODE_WHILE, NODE_FOR, NODE_FUNCTION, NODE_BLOCK:
    struct ASTNode *body;
    // ...
} ASTNode;
```

Every node is created with `arena_calloc(p->arena, 1, sizeof(ASTNode))` — allocated from the arena, zeroed out (`calloc` = `malloc` + `memset 0`), and automatically freed by the next `arena_reset()`.

#### Interview Answer

> "The parser is a recursive descent parser — each grammar rule maps to one C function, and the call hierarchy naturally encodes operator precedence. Pipelines bind tighter than &&/||, because parse_pipeline is called by parse_list, not the other way around. Every AST node is arena-allocated, so the entire tree — which could be dozens of nodes for a complex if/elif/fi — disappears with one arena_reset() after execution. Recursive descent is the same technique used in GCC, Clang, and most production language compilers."

---

### MODULE 5 — executor.c (The Command Runner)

**File:** [src/executor.c](src/executor.c)

#### What It Does

executor.c receives the root of the AST tree and walks it, doing what each node type says.

```c
int executor_execute(Shell *shell, ASTNode *node) {
    switch (node->type) {
    case NODE_COMMAND:    return exec_command(shell, node);
    case NODE_PIPELINE:   return exec_pipeline(shell, node);
    case NODE_AND:        return exec_and(shell, node);
    case NODE_OR:         return exec_or(shell, node);
    case NODE_SEQUENCE:   return exec_sequence(shell, node);
    case NODE_BACKGROUND: return exec_background(shell, node);
    case NODE_IF:         return exec_if(shell, node);
    case NODE_WHILE:      return exec_while(shell, node);
    case NODE_FOR:        return exec_for(shell, node);
    }
}
```

Each case is simple to implement because the tree already has the right structure.

#### Running a Single Command — Fork/Exec

For `ls -la`:

```c
static int exec_command(Shell *shell, ASTNode *node) {
    // 1. Is it a builtin? (cd, export, exit, etc.)
    if (is_builtin(node->args[0])) {
        return run_builtin(shell, node);  // run in-process, no fork
    }

    // 2. Fork a child process
    pid_t pid = fork();

    if (pid == 0) {
        // === CHILD PROCESS ===
        setpgid(0, 0);           // put child in its own process group
        child_reset_signals();   // reset signal handlers to defaults

        // Apply I/O redirections (open files, dup2 to correct fds)
        apply_redirections(node->redirs);

        // Replace child process with ls
        execvp(node->args[0], node->args);

        // If we reach here, execvp failed
        perror("vsh: exec");
        exit(127);

    } else {
        // === PARENT PROCESS ===
        setpgid(pid, pid);         // also set group (race condition prevention)
        job_add(shell, pid, ...);  // track this job
        job_wait_foreground(pid);  // wait for it to finish
    }
}
```

#### Builtins — Why cd Can't Fork

`cd /home/jake` changes the current directory. The C function for this is `chdir()`.

If the executor forked a child and ran `chdir()` in the child:
- Child's working directory changes
- Child exits
- Parent (shell) never changed its directory

So `cd` must run **inside the shell process directly**, not in a child. Same for `export VAR=value`, `unset`, `exit`, `source`, `history`, `jobs`.

These are **builtins** — checked first, before any fork.

#### IO Redirections

For `ls > output.txt`:

```c
// In the child, before exec:
int fd = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
//                           ^ write-only  ^ create if missing  ^ erase existing
dup2(fd, STDOUT_FILENO);  // fd 1 now points to output.txt
close(fd);                // close the original fd (we have it as 1 now)
// execvp("ls") → ls writes to fd 1 → goes to output.txt
```

For `grep foo < input.txt`:
```c
int fd = open("input.txt", O_RDONLY);
dup2(fd, STDIN_FILENO);  // fd 0 now reads from input.txt
close(fd);
// execvp("grep") → grep reads fd 0 → reads input.txt
```

#### The Known Bug — exec_function()

Shell functions (`myfunc() { ls; }`) are broken. The code does this:

```c
// When a function is defined:
char ptr_buf[32];
snprintf(ptr_buf, sizeof(ptr_buf), "%p", (void *)fn->body);  // store pointer as string
setenv(func_name, ptr_buf, 1);  // save in env

// When the function is called later:
void *ptr = /* parse hex string back to pointer */;
ASTNode *body = (ASTNode *)ptr;  // DANGLING POINTER! arena was reset!
executor_execute(shell, body);   // SEGFAULT
```

The AST node `fn->body` was arena-allocated. `arena_reset()` freed it between the function definition and the function call. The saved pointer now points to freed memory. Instant crash.

**How to fix it:** Store function bodies in a separate persistent hash map with `malloc`'d copies of the AST, outside the parse arena.

#### Interview Answer

> "The executor is an AST walker — a switch dispatches on node type, and each case handles one construct. Simple commands go through fork/exec; builtins run in-process because they need to modify shell state (cd changes directory, export adds to the environment). IO redirection works by opening the target file in the child before exec, then using dup2() to point the appropriate file descriptor to that file. The known bug is in function definitions — the AST pointer is serialized to an env var, but arena_reset() makes it dangling. The fix is a persistent hash map for function bodies, separate from the parse arena."

---

### MODULE 6 — pipeline.c (Connecting Programs)

**File:** [src/pipeline.c](src/pipeline.c)

#### What It Does

For `ls | grep .txt | wc -l` (3 commands), pipeline.c:
1. Creates 2 pipes (N-1 pipes for N commands)
2. Forks 3 children
3. Wires each child's stdin/stdout to the correct pipe ends using dup2
4. Closes all pipe ends in the parent (critical — explained below)
5. Waits for all children to finish

#### The Pipe Wiring

```
          ls              grep            wc
          │               │               │
stdout ───┤               │               │
          └──→ pipe[0] ───┤               │
                 [1]    [0]               │
               (write) (read)            │
                         └──→ pipe[1] ───┤
                               [1]    [0]
                             (write) (read)
                                       └──→ terminal
```

For the *middle* command (grep, at index i=1):

```c
// grep reads from pipe[0] (ls's output)
dup2(pipes[0][0], STDIN_FILENO);   // stdin = pipe[0] read end

// grep writes to pipe[1] (wc's input)
dup2(pipes[1][1], STDOUT_FILENO);  // stdout = pipe[1] write end

// Close ALL original pipe file descriptors
// (we've already redirected — the originals are now redundant)
for (int j = 0; j < n-1; j++) {
    close(pipes[j][0]);
    close(pipes[j][1]);
}
```

#### Why Closing Pipe Ends in the Parent Is Critical

This is the most common pipe bug, and interviewers love asking about it.

```
ls | wc -l
```

After forking both children, the parent still has both ends of the pipe open in its file descriptor table. Let's trace what happens:

```
Writers of pipe[0][1]:   ls (child),  parent
Readers of pipe[0][0]:   wc (child)

ls finishes and exits → ls's pipe[0][1] is closed
BUT parent still has pipe[0][1] open!

wc is waiting for EOF on its stdin (pipe[0][0])
EOF happens only when ALL writers close their write end
Parent holds pipe[0][1] open → wc NEVER gets EOF → wc hangs forever
→ Shell hangs forever
```

**Fix:** after forking all children, the parent must immediately close every pipe file descriptor.

```c
// After all forks:
for (int i = 0; i < n - 1; i++) {
    close(pipes[i][0]);   // close read end in parent
    close(pipes[i][1]);   // close write end in parent
}
```

Now only the child processes hold the pipe ends. When they finish, the pipes close, and the next program gets EOF naturally.

#### Interview Answer

> "For N piped commands, pipeline.c creates N-1 pipe pairs. Each child gets dup2'd to the correct read and write ends. The critical detail is closing all pipe ends in the parent after forking all children. If the parent holds a write end open, the downstream command never gets EOF on its stdin and hangs forever — the shell deadlocks. Same topology appears in kernel driver data paths and DMA descriptor ring buffers."

---

### MODULE 7 — job_control.c (Ctrl+Z, fg, bg)

**File:** [src/job_control.c](src/job_control.c)

#### What Is Job Control?

When you run `sleep 100`, you want to be able to:
- Press Ctrl+Z to pause it
- Type `bg` to resume it in the background
- Type `fg` to bring it back to the foreground
- Run multiple background jobs simultaneously

This requires **process groups** and **terminal ownership**.

#### Process Groups

Every process belongs to a **process group** (identified by a pgid — process group ID). The terminal sends signals to the *entire* foreground process group at once, not to individual processes.

```
Shell process group (pgid=1000)
  └── vsh (pid=1000)

Job 1: ls pipeline (pgid=1001)
  ├── ls    (pid=1001)
  └── wc -l (pid=1002)

Job 2: sleep (pgid=1003)
  └── sleep (pid=1003)
```

When you press Ctrl+Z, the terminal sends SIGTSTP to every process in the *foreground* process group. That stops the entire pipeline, not just one command.

#### Shell Initialization

```c
void job_control_init(Shell *shell) {
    pid_t shell_pid = getpid();

    // Put the shell in its own process group
    setpgid(shell_pid, shell_pid);

    // Make the shell the foreground process group (grab the terminal)
    tcsetpgrp(STDIN_FILENO, shell_pid);

    // Shell ignores job-control signals (so Ctrl+Z doesn't pause the shell itself)
    signal(SIGTSTP, SIG_IGN);   // Ctrl+Z
    signal(SIGTTIN, SIG_IGN);   // background process trying to read terminal
    signal(SIGTTOU, SIG_IGN);   // background process trying to write terminal

    // Install SIGCHLD handler (notified when any child changes state)
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}
```

#### Running a Foreground Job

```c
void job_wait_foreground(Shell *shell, pid_t pgid) {
    // Hand the terminal to the job's process group
    tcsetpgrp(STDIN_FILENO, pgid);

    // Wait for the job to stop or finish
    int status;
    waitpid(-pgid, &status, WUNTRACED);
    //      ↑ negative pgid = wait for any process in that group

    // Take the terminal back when done
    tcsetpgrp(STDIN_FILENO, shell->shell_pid);
}
```

The `tcsetpgrp()` call "gives" the terminal to a process group. Only the foreground group can read from the terminal (others get SIGTTIN). Giving it back after waiting ensures the shell regains control of keyboard input.

#### SIGCHLD Handler — Signal-Safe Code

When a child changes state (exits, stops, continues), the kernel sends SIGCHLD to the shell. The signal handler runs asynchronously — interrupting whatever function the shell was in the middle of executing.

```c
static void sigchld_handler(int sig) {
    int saved_errno = errno;  // MUST save — signal can interrupt any function that uses errno

    pid_t pid;
    int status;
    // Loop because multiple children may have changed state simultaneously
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        // Find the job with this pid and update its state
        update_job_state(g_shell, pid, status);
    }

    errno = saved_errno;  // MUST restore
}
```

**Why save/restore errno?**

`errno` is a global (or thread-local) variable set by system calls when they fail. If the signal handler interrupts a function that's about to check `errno`, and the handler calls `waitpid()` which modifies `errno`, the original function reads the wrong error code. This causes silent, hard-to-debug failures. Always save/restore in signal handlers.

**Why WNOHANG?**

`waitpid()` normally blocks until the child exits. In a signal handler, you never block — the signal handler must return quickly. `WNOHANG` makes `waitpid()` return immediately if no child has changed state yet.

**Why SA_RESTART?**

Without `SA_RESTART`, a system call like `read()` (used in the line editor) returns -1 with `errno=EINTR` whenever a signal arrives. With `SA_RESTART`, the system call automatically retries instead of returning an error. This prevents the line editor from crashing every time a background job exits.

#### Interview Answer

> "Job control works through process groups. The shell assigns each new pipeline its own process group via setpgid(). The terminal is 'given' to a process group with tcsetpgrp() — Ctrl+Z sends SIGTSTP to the entire foreground group, not just one process. The SIGCHLD handler loops with WNOHANG to reap all children without blocking. errno must be saved and restored in signal handlers because it's a global that can be overwritten mid-function — a subtle but critical detail. SA_RESTART prevents system calls from failing with EINTR when a background job exits."

---

### MODULE 8 — vsh_readline.c (The Custom Line Editor)

**File:** [src/vsh_readline.c](src/vsh_readline.c)

#### What Problem Does It Solve?

Normally when you type in a terminal, the OS buffers your input and sends a complete line to the program only when you press Enter (called "canonical mode" or "cooked mode"). The program gets the string "ls -la\n" and has no idea what keys you pressed along the way.

But for a real line editor with arrow keys, backspace, Ctrl+A, tab completion, and history, you need to see *every* keypress as it happens. This requires **raw mode**.

#### Raw Mode

```c
// Save old settings
struct termios old_termios;
tcgetattr(STDIN_FILENO, &old_termios);

// Switch to raw mode
struct termios raw = old_termios;
raw.c_lflag &= ~(ECHO | ICANON);  // disable echo and line buffering
// ECHO   = terminal echoing typed chars back to screen (we'll do it manually)
// ICANON = wait for newline before sending input to program
tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

// ... now every keypress arrives immediately ...

// Restore when done
tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
```

In raw mode, every keypress arrives as a byte (or multi-byte sequence). The line editor reads each one and decides what to do.

#### ANSI Escape Sequences

Arrow keys don't send a single byte — they send a 3-byte sequence:

```
↑ = ESC [ A = 0x1B 0x5B 0x41
↓ = ESC [ B
→ = ESC [ C
← = ESC [ D
```

The editor reads the first byte. If it's ESC (0x1B), it reads two more bytes to determine which special key was pressed.

To move the cursor on screen, you write these sequences to stdout:

```c
// Move cursor left 1 column
write(STDOUT_FILENO, "\033[D", 3);

// Erase entire current line
write(STDOUT_FILENO, "\033[2K", 4);

// Move cursor to column 0
write(STDOUT_FILENO, "\033[0G", 4);
```

**Why `write()` instead of `printf()`?**

`printf()` uses buffered I/O — it holds characters in an internal buffer and flushes them when convenient. In a signal handler, `printf()` is not safe to call (it uses global state that can be corrupted). `write()` is a direct system call — safe, immediate, no buffering.

#### The Edit Buffer

```c
typedef struct {
    SafeString *buf;      // the text being typed
    size_t      cursor;   // cursor position (index into buf)
    char       *prompt;   // the "vsh> " prefix
    size_t      prompt_len; // length of prompt in terminal columns
} LineEditor;
```

Every keypress modifies `buf` and `cursor`, then redraws the entire line using ANSI sequences:

```
1. Move cursor to column 0
2. Erase the entire line
3. Write the prompt
4. Write the buffer content
5. Move cursor to position (prompt_len + cursor)
```

#### Known Bug — Colored Prompts

```c
// Line 787 in vsh_readline.c
ed.prompt_len = (int)strlen(prompt);  // WRONG for colored prompts
```

If the prompt contains ANSI color codes like `\033[32mvsh>\033[0m` (green "vsh>"), `strlen()` counts all bytes including the escape sequences. But escape sequences don't occupy terminal columns — they're invisible control codes. `strlen` says the prompt is 15 characters wide, but on screen it's only 5 characters wide.

This makes cursor positioning wrong — the editor thinks the cursor is at column 15+cursor when it's actually at 5+cursor. The line editor breaks for colored prompts.

#### Features Implemented

- **↑↓ history navigation** — stored in a ring buffer, accessed with arrow keys
- **Kill ring** — Ctrl+K cuts text after cursor, Ctrl+Y pastes it back
- **Ctrl+A / Ctrl+E** — jump to start/end of line
- **Ctrl+W** — delete word before cursor (using `memmove` for overlap safety)
- **Reverse-i-search** — Ctrl+R searches history interactively
- **Tab completion** — queries PATH for executables, current directory for files, shows columnar menu

#### Interview Answer

> "The line editor puts the terminal in raw mode by disabling ECHO and ICANON via tcsetattr(). Every keypress arrives as bytes — arrow keys as 3-byte ESC sequences that we parse explicitly. Screen redraw uses ANSI escape codes to erase and rewrite the current line. I use write() instead of printf() because write() is safe to call from signal handlers and avoids buffering issues. The known bug is using strlen() on the prompt to calculate cursor column offset — strlen counts ANSI escape bytes which are invisible and don't occupy screen columns, so colored prompts break cursor positioning."

---

## PART 4: What You Actually Learn From This Project

| Concept | Where in vsh | Why it matters for Airspan |
|---|---|---|
| Memory management without GC | arena.c, safe_string.c | Embedded has no garbage collector |
| Pointer arithmetic, alignment | align_up() in arena.c | Cache-line alignment in hardware interfaces |
| Process lifecycle (fork/exec/wait) | executor.c, pipeline.c | Linux-based DU/CU process management |
| Signal handling — async-safe code | job_control.c | IRQ handlers in embedded/kernel code |
| File descriptors and dup2 | executor.c, pipeline.c | UNIX sockets, network FDs in 5G stack |
| Terminal I/O, raw mode, UART | vsh_readline.c | Serial/UART drivers in embedded |
| Finite state machines | lexer.c | Protocol parsers, frame decoders |
| Tree data structures | parser.c, executor.c | Config trees, ASN.1 parsing |
| Recursive descent parsing | parser.c | AT command parsing, O-RAN message parsing |
| C structs and pointers, no STL | all modules | Embedded C has no C++ STL |

### The Embedded Systems Connection

Arena allocator → **RTOS memory pools** (FreeRTOS `pvPortMalloc`, Zephyr `k_mem_pool`). Same idea: pre-allocated regions, no heap fragmentation.

Signal handlers → **Interrupt Service Routines (ISRs)**. Same rules: save registers (errno), don't call blocking functions, return fast.

Raw terminal I/O → **UART/serial drivers**. Same concepts: configure hardware registers, read bytes one at a time, handle special byte sequences.

Pipe EOF behavior → **DMA buffer descriptors**. Same principle: a consumer must not block waiting for input if no producer holds the channel open.

---

## PART 5: Honest Resume Audit

| Resume Claim | Verdict | Honest Detail |
|---|---|---|
| "C from scratch, zero external dependencies" | ✅ True | Only `-lm` (standard C math lib). Verified in Makefile. |
| "Custom line editor without readline/ncurses" | ✅ True | 950 lines of raw terminal I/O in vsh_readline.c |
| "Arena allocator for zero-leak recursive descent parsing" | ✅ True | Genuinely well-implemented, O(1) alloc and reset |
| "Custom bounds-checked SafeString library" | ✅ True | Proper 2× growth, double-pass vsnprintf, bounds-checked |
| "Job control (signals, fg/bg)" | ✅ True | Real SIGCHLD, process groups, tcsetpgrp, SA_RESTART |
| "Pipelines, IO redirection, AST-based command dispatch" | ✅ Mostly true | Heredoc `<<` is parsed but executor stubs it with "not yet implemented" |
| "Memory-safe" | ⚠️ Mostly true | exec_function() has a real dangling pointer bug |
| "POSIX-compatible" | ⚠️ Stretch | Say "POSIX-style" instead. Missing: `$()` command substitution, `$(())` arithmetic, `trap` |

### What to Say About the Bugs

**exec_function() bug:** "Shell functions have a known bug. The function body is an arena-allocated AST node — I stored its pointer as a hex string in environment variables to look it up later. But arena_reset() runs after every command, making that pointer dangling. Calling a user-defined function would segfault. The fix is to store function bodies in a persistent hash map with malloc'd copies of the AST, separate from the parse arena."

**Heredoc:** "Heredoc redirections (<<EOF ... EOF) are parsed — the token type exists — but the executor prints 'not yet implemented'. It's about 50 lines of work using a temporary file or memfd."

---

## PART 6: Full Interview Q&A Bank

### Memory

**Q: What is a memory leak? How does vsh prevent them in the parser?**
A: A memory leak is allocated heap memory that's never freed — the pointer is lost and `free()` can never be called. Over time (in a long-running shell), leaks accumulate and exhaust memory. vsh prevents this by arena-allocating all tokens and AST nodes. After each command executes, `arena_reset()` frees the entire parse arena in one call — no individual frees needed, so nothing can be forgotten.

**Q: What is a dangling pointer?**
A: A pointer to memory that has already been freed. Dereferencing it causes undefined behavior — typically a crash or silent data corruption. The exec_function() bug in vsh is exactly this: the AST body is arena-allocated, `arena_reset()` frees it, but the stored address is read back later as a function body.

**Q: What is buffer overflow?**
A: Writing more data to a fixed-size buffer than it can hold, overwriting adjacent memory. In C, the compiler doesn't stop you. SafeString prevents this by checking capacity before every write and reallocating if needed.

**Q: What is stack memory vs heap memory?**
A: Stack memory is automatically allocated when a function is called and freed when it returns — variables declared inside a function live on the stack. Heap memory (malloc/free) has a lifetime you control explicitly. Stack is fast but small (typically 8MB). Heap is larger but requires manual management. In vsh, token strings and AST nodes go on the heap (via the arena) because they must outlive the functions that create them.

**Q: What is memory alignment and why does vsh's arena care about it?**
A: Alignment means data starts at an address divisible by its size (or the platform's preferred size). A 4-byte int should start at an address divisible by 4. CPUs are faster (or on some architectures only work) with aligned data. The arena's `align_up()` function rounds every allocation size up to the next multiple of 8, ensuring subsequent allocations are always aligned.

### OS / Linux

**Q: What does fork() return?**
A: It returns in two processes simultaneously. In the parent: the child's PID (a positive integer). In the child: 0. On failure (before the child is created): -1 to the caller only.

**Q: What is the difference between exec() and fork()?**
A: `fork()` creates a copy of the current process. `exec()` replaces the current process with a new program. To run a new program without destroying the shell: fork first (creating a copy), then exec in the copy. The original process (shell) survives.

**Q: What is a pipe and how does dup2 work?**
A: A pipe is a unidirectional in-kernel buffer with two file descriptor ends — one for writing (write end), one for reading (read end). `dup2(old_fd, new_fd)` makes `new_fd` refer to the same open file as `old_fd`, closing `new_fd` first if it was open. To pipe `ls | grep`, you redirect ls's stdout (fd 1) to the pipe's write end, and grep's stdin (fd 0) to the pipe's read end.

**Q: Why must the parent close pipe ends after forking?**
A: EOF on a pipe's read end happens only when all write ends are closed. If the parent holds a write end open, the downstream reader never gets EOF and blocks forever — the shell deadlocks. The parent must close all pipe ends immediately after forking all children.

**Q: What is SIGCHLD?**
A: A signal sent by the kernel to a parent when one of its children changes state (exits, stops with SIGTSTP, or continues with SIGCONT). The shell installs a SIGCHLD handler to update its job table. The handler uses WNOHANG to avoid blocking, and loops until waitpid() returns -1 or 0, because multiple children may have changed state simultaneously.

**Q: Why must signal handlers save and restore errno?**
A: errno is a global variable set by failed system calls. A signal handler runs asynchronously — it can interrupt any function, including one that just received an error and is about to check errno. If the handler calls functions that modify errno (like waitpid), the interrupted function reads the wrong error code. Saving at start and restoring at end prevents this.

**Q: What is SA_RESTART?**
A: A flag for sigaction() that makes slow system calls (read, write, select) automatically restart if interrupted by a signal, instead of returning -1 with errno=EINTR. Without it, the line editor's read() would fail every time a background job exits, requiring explicit retry logic throughout.

**Q: What is a process group and why does the shell use them?**
A: A process group is a collection of processes that share a pgid. The terminal sends signals (SIGINT, SIGTSTP) to the entire foreground process group. The shell assigns each pipeline its own process group with `setpgid()`, so Ctrl+Z stops all commands in a pipeline together. `tcsetpgrp()` transfers terminal ownership to a process group.

**Q: What is tcsetpgrp() and when is it called?**
A: `tcsetpgrp(fd, pgid)` makes the process group `pgid` the foreground group for the terminal at `fd`. The shell calls it before waiting for a foreground job (giving the terminal to the job), and again after the job finishes (taking the terminal back). This ensures keyboard input goes to the right process.

### The Project

**Q: Walk me through what happens when you type `ls | wc -l` in vsh.**
A: vsh_readline reads the string character by character in raw mode. `arena_reset()` clears the parse arena. `lexer_tokenize()` produces: WORD:"ls", PIPE, WORD:"wc", WORD:"-l", EOF. `parser_parse()` builds a PIPELINE node with two COMMAND children. The executor dispatches to pipeline.c. pipeline.c creates 1 pipe (fds[0] and fds[1]), forks child 1 (ls), wires its stdout to fds[1] with dup2, forks child 2 (wc), wires its stdin to fds[0] with dup2, closes both fds in the parent. Both children run: ls writes filenames into the pipe, wc counts lines from the pipe, wc prints the count to stdout. The parent calls waitpid for both. `arena_reset()` frees all tokens and nodes.

**Q: How does `cd /home/jake` work differently from `ls`?**
A: `cd` is a builtin. The executor checks if the command name matches the builtin table before forking. If it does, it calls the builtin function directly in the shell process. `chdir()` (the C function for cd) only changes the directory of the process that calls it. If cd ran in a forked child, the child would change directories and exit, leaving the shell's directory unchanged. Builtins that modify shell state must run in-process.

**Q: What would you fix first in vsh?**
A: The exec_function() dangling pointer bug — shell functions are completely broken. Fix: a persistent hash map (separate from the parse arena) storing malloc'd copies of function bodies. Second: implementing heredoc (<<), which is parsed but stubbed in the executor. Third: fixing the prompt_len calculation to strip ANSI escape sequences before measuring column width.

---

## PART 7: 4-Minute Interview Pitch

*Practice saying this out loud. Time yourself.*

---

"I built vsh — a POSIX-style shell in C from scratch, about 4000 lines with zero external dependencies. Not even readline.

The core is a four-stage pipeline. A custom line editor reads raw terminal keystrokes using raw mode via tcsetattr. A lexer tokenizes the input — handling quoting, escape sequences, and greedy multi-character operator matching. A recursive descent parser builds an abstract syntax tree. An executor walks the tree and runs everything — forking processes, connecting pipes, handling builtins in-process.

The memory system is the part I find most interesting technically. All tokens and AST nodes are allocated from an arena — a page-based bump allocator. Allocation is O(1) pointer-bump. After each command executes, arena_reset() frees everything in O(1). One call, zero individual frees, zero possible memory leaks in the parser. This is the same pattern as RTOS memory pools.

For job control, I implemented the full Unix model: setpgid for process groups, tcsetpgrp for terminal ownership, a SIGCHLD handler with SA_RESTART and WNOHANG so background jobs don't interrupt the line editor. Signal handlers save and restore errno to prevent corruption.

The honest limitation is that shell functions have a dangling pointer bug — I serialized the AST pointer to an environment variable, but arena_reset() makes it invalid before the function is ever called. The fix is a persistent hash map for function bodies outside the parse arena.

For Airspan, the arena maps directly to embedded memory pools. The signal handler discipline is identical to ISR coding rules — no blocking, save volatile state, return fast. The pipe management and process lifecycle work is what embedded Linux systems do constantly at the process coordination layer."

---

*That's the whole project. Each module connects to the next. Read this once, then read the actual source files for the modules assigned on your study days.*
