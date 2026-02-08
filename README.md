# vsh — Vanguard Shell

A full-featured, memory-safe Linux shell written in C from scratch. No external dependencies beyond the C standard library and POSIX APIs — no readline, no ncurses, no third-party libraries.

## Features

**Core Shell**
- POSIX-compatible command execution with modern extensions
- Pipelines, AND/OR chains, sequences, background jobs
- Input/output/append redirections with fd targeting (`2>&1`)
- Single and double quoting, backslash escapes, comments
- `if`/`then`/`elif`/`else`/`fi`, `while`/`do`/`done`, `for`/`in`/`do`/`done`
- Shell functions, subshells, block grouping
- Variable expansion (`$VAR`, `${VAR:-default}`, `$?`, `$$`, `$#`, `$@`)
- Tilde expansion and glob/wildcard matching
- Alias expansion with recursive detection
- History expansion (`!!`, `!N`, `!-N`, `!prefix`)
- RC file support (`~/.vshrc` sourced on interactive startup)

**Line Editor** (custom implementation, no libreadline)
- Cursor movement (Home, End, arrow keys, Ctrl+A/E/B/F)
- Kill/yank (Ctrl+K/U/W/Y)
- Reverse incremental search (Ctrl+R)
- Tab completion for files, directories, commands, and builtins
- History navigation (Up/Down, prefix search)

**Prompt**
- Powerline-style two-line prompt
- Time, user@host, shortened working directory
- Git branch detection (walks up directory tree)
- Color-coded exit status indicator

**Job Control**
- Background jobs with `&`
- `fg`, `bg`, `jobs` builtins
- Process groups, terminal control, `SIGCHLD` reaping
- Stopped job warnings on exit

**Memory Safety**
- Arena allocator for per-command parse trees (zero-leak parsing)
- Bounds-checked dynamic string buffer (`SafeString`)
- Compiled with `-Wall -Wextra -Werror -Wshadow` and sanitizer support

## Builtin Commands

| Command | Description |
|---------|-------------|
| `cd` | Change directory (supports `~`, `-`, `OLDPWD`) |
| `pwd` | Print working directory |
| `echo` | Print text (`-n`, `-e` flags) |
| `export` | Set/display exported variables |
| `unset` | Remove a variable |
| `alias` / `unalias` | Define or remove aliases |
| `history` | Display/manage command history (`-c`, `-n N`) |
| `source` / `.` | Execute commands from a file |
| `type` | Describe a command (builtin, alias, or external) |
| `jobs` / `fg` / `bg` | Job control |
| `pushd` / `popd` / `dirs` | Directory stack |
| `exit` | Exit the shell |
| `help` | Display builtin help |
| `return` | Return from a function |
| `local` | Declare a local variable |

### Showcase Builtins

| Command | Description |
|---------|-------------|
| `sysinfo` | Colored system dashboard (OS, kernel, CPU, memory, disk, uptime) |
| `httpfetch` | Raw socket HTTP GET with redirect following |
| `calc` | Recursive descent math evaluator with functions (`sin`, `cos`, `sqrt`, `log`, etc.) and constants (`pi`, `e`) |
| `watch` | Repeat command execution at intervals (`watch -n 2 date`) |
| `colors` | 256-color palette and true-color gradient display |

## Building

Requires GCC and GNU Make. No other dependencies.

```bash
make release    # Optimized binary → ./vsh
make debug      # Debug build with symbols → ./vsh_debug
make sanitize   # AddressSanitizer + UBSan → ./vsh_debug
make test       # Build and run unit tests (215 tests)
make clean      # Remove all build artifacts
```

## Usage

```bash
# Interactive mode
./vsh

# Run a single command
./vsh -c "echo hello | wc -c"

# Run a script
./vsh script.sh

# Install to /usr/local/bin
sudo make install
```

## Configuration

Create `~/.vshrc` for startup configuration:

```bash
# Aliases
alias ll='ls -la'
alias gs='git status'
alias ..='cd ..'

# Environment
export EDITOR=vim
export PATH=$HOME/bin:$PATH
```

## Project Structure

```
include/          # Header files (13 files)
  arena.h           Arena allocator interface
  builtins.h        Builtin command registry
  env.h             Environment variable table
  executor.h        AST executor
  history.h         Command history
  job_control.h     Job control (fg/bg/jobs)
  lexer.h           Tokenizer
  parser.h          Recursive descent parser
  pipeline.h        Pipeline execution
  safe_string.h     Bounds-checked string buffer
  shell.h           Shell state and types
  vsh_readline.h    Line editor
  wildcard.h        Glob/wildcard matching

src/              # Implementation (30 files, ~8800 lines)
  main.c            Entry point and CLI options
  shell.c           REPL loop, prompt, signals, RC file loading
  lexer.c           Tokenizer
  parser.c          Recursive descent parser → AST
  executor.c        AST dispatch, expansion, builtin routing
  pipeline.c        Pipe creation and process group wiring
  env.c             Hash table + expansion engine
  job_control.c     Process groups, terminal control, SIGCHLD
  history.c         Ring buffer with persistence
  vsh_readline.c    Custom line editor with tab completion
  arena.c           Page-based bump allocator
  safe_string.c     Dynamic string buffer
  wildcard.c        POSIX glob + custom matching
  builtins.c        Builtin registry and dispatch
  builtins/         Individual builtin implementations (16 files)

tests/            # Unit tests (6 files, 215 tests)
  test.h            Test framework macros
  test_main.c       Test runner
  test_arena.c      Arena allocator tests
  test_safe_string.c Safe string tests
  test_lexer.c      Lexer tests
  test_parser.c     Parser tests
```

## License

MIT
