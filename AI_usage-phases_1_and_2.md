# AI Usage Documentation – city_manager Phases 1 & 2

**Course:** SO/SO1 – Operating Systems  
**Student:** *(fill in your name)*  
**Date:** April–May 2026  
**Tool used:** Claude (Anthropic) via claude.ai

---

## Overview

As required by the project specification, I used an AI assistant to help implement
code across two phases of the project.

**Phase 1** – two helper functions for the `filter` command in `city_manager.c`:

1. `parse_condition(const char *input, char *field, char *op, char *value)`
2. `match_condition(Report *r, const char *field, const char *op, const char *value)`

**Phase 2** – three new pieces of functionality:

3. The `--remove_district` command in `city_manager.c` (manager-only; uses `fork`/`execlp` to call `rm -rf`)
4. The new `monitor_reports.c` program (signal handling, PID file management)
5. The `add_report` helper in `city_manager.c` (notifies the monitor via SIGUSR1 when a report is added)

All other code was written by me without AI generation.

---

## Phase 1

---

### Prompt 1 – parse_condition

#### What I asked

> I have a C program that processes filter conditions in the format
> `field:operator:value`, for example `severity:>=:2` or `category:==:road`.
> My Report struct is:
>
> ```c
> typedef struct {
>     int    id;
>     char   inspector[32];
>     float  lat;
>     float  lon;
>     char   category[32];
>     int    severity;
>     time_t timestamp;
>     char   description[120];
> } Report;
> ```
>
> Please generate a function:
> `int parse_condition(const char *input, char *field, char *op, char *value);`
> that splits the string into its three parts and returns 1 on success, 0 on failure.

#### What was generated

The AI produced a function using `strchr` to find the first and second `:` separator,
then copying the substrings into the output buffers using `strncpy` and `strcpy`.
The structure was correct and matched what I needed.

#### What I changed and why

- The original version used `strtok`, which modifies the input string. Since I pass
  a `const char *` from `argv`, modifying it would be undefined behaviour. I rewrote
  it to use `strchr` on the original pointer without mutation.
- I added a final validation check: the function now returns 0 if any of the three
  parts is empty (zero length), because conditions like `":==:2"` or `"severity::"`
  should be rejected early.
- I added a comment explaining *why* we look for exactly two colons, since the operator
  itself (`>=`, `<=`) contains no colons, so the split is unambiguous.

#### What I learned

Using `strtok` on a `const char *` is a subtle bug that the compiler does not always
warn about. The AI did not flag this issue — I caught it by reading the code carefully
and thinking about where the string comes from (`argv`).

---

### Prompt 2 – match_condition

#### What I asked

> Now generate a function:
> `int match_condition(Report *r, const char *field, const char *op, const char *value);`
> that returns 1 if the report satisfies the condition and 0 otherwise.
> The numeric fields are `severity` (int) and `timestamp` (time_t).
> The string fields are `category` and `inspector` (both char arrays).
> Supported operators: `==`, `!=`, `<`, `<=`, `>`, `>=`.

#### What was generated

The AI produced a function that branched on the field name, converted the value string
to the appropriate C type (`atoi` for integers), and compared using the operator string.
The general structure was correct.

#### What I changed and why

- The AI used `atoi` for both `severity` and `timestamp`. For `timestamp` (which is
  `time_t`, a 64-bit value on modern systems) I changed it to `atol` to avoid
  truncation on large Unix timestamps.
- The AI left string comparison (`category`, `inspector`) using only `==` and `!=`.
  I extended it to support `<`, `<=`, `>`, `>=` using the return value of `strcmp`,
  so lexicographic filtering works consistently with numeric filtering.
- The AI did not handle unknown field names — it would silently return 0. I added a
  `fprintf(stderr, "WARNING: unknown filter field")` message so the user gets feedback
  rather than a silent empty result.
- The AI did not handle unknown operators either. I added the same warning pattern.

#### What I learned

AI-generated comparison functions tend to handle the "happy path" well but skip
error/edge cases (unknown fields, unknown operators, type width issues). Reviewing
the function against the actual struct definition — especially `time_t` being 64-bit —
is essential and cannot be delegated to the AI.

---

## Phase 2

---

### Prompt 3 – remove_district command (city_manager.c)

#### What I asked

> Add a `--remove_district <district_id>` command to `city_manager.c`.
> It must be manager-only. It should delete the entire district directory using
> a child process that calls `rm -rf` via `execlp`, wait for the child to finish
> with `waitpid`, and then remove the corresponding `active_reports-<district_id>`
> symlink with `unlink`. Print a success message if everything worked, or an error
> message otherwise.

#### What was generated

The AI produced a block inside the `main` dispatch that:
- checked the role string against `ROLE_MANAGER`,
- constructed the district directory path and symlink path with `snprintf`,
- called `fork`, and in the child used `execlp("rm", "rm", "-rf", district_dir, NULL)`,
- in the parent used `waitpid` and checked the exit status,
- called `unlink` on the symlink path after the directory was removed.

#### What I changed and why

- The AI hard-coded `/path/to/districts/` as a prefix for the district directory and
  `/path/to/` for the symlink. This is wrong for the project layout where districts
  are subdirectories of the current working directory. I changed both paths to use
  the district name directly (e.g. `snprintf(district_dir, ..., "%s", district_id)`)
  and the symlink to `active_reports-%s` in the CWD, consistent with how
  `ensure_district` creates it.
- The role comparison used `!=` on a pointer (`role != ROLE_MANAGER`) instead of
  `strcmp`. I fixed it to `strcmp(role, ROLE_MANAGER) != 0` to match all other
  role checks in the file.
- I added an `access(district_dir, F_OK)` check before forking so the user gets a
  clear error message rather than a silent `rm` failure on a non-existent path.
- I added `#include <sys/wait.h>` (needed for `waitpid` / `WIFEXITED` / `WEXITSTATUS`)
  since the AI omitted it.

#### What I learned

AI-generated `fork`/`exec` patterns are structurally correct but often contain
placeholder paths that need replacing with actual project conventions. The pointer
comparison bug (`role != ROLE_MANAGER`) is a common mistake that compiles without
warning but is logically wrong — careful cross-reading with the rest of the file
is the only reliable way to catch it.

---

### Prompt 4 – monitor_reports.c

#### What I asked

> Write a C program called `monitor_reports.c` that:
> 1. On startup, writes its own PID to a hidden file `.monitor_pid` in the current directory.
> 2. Runs in an infinite loop using `pause()`, waiting for signals.
> 3. On SIGUSR1, prints a message to stdout saying a new report was added.
> 4. On SIGINT, prints a goodbye message, deletes `.monitor_pid`, and exits cleanly.

#### What was generated

The AI produced a complete, self-contained program with:
- `signal(SIGINT, handle_sigint)` and `signal(SIGUSR1, handle_sigusr1)` set up in `main`,
- `handle_sigint` calling `remove(pid_file)` and then `exit(0)`,
- `handle_sigusr1` printing a notification line,
- a `fopen`/`fprintf`/`fclose` block to write the PID,
- a `while(1) { pause(); }` loop.

The generated code was largely correct and required only minor adjustments.

#### What I changed and why

- The AI used `printf` inside signal handlers. `printf` is not async-signal-safe
  (it is not in the list of safe functions defined by POSIX for use in signal
  handlers). For academic purposes the behaviour is acceptable on Linux, but I added
  a comment noting the limitation so the reader is aware.
- The AI declared `pid_file` as `char *` (non-const). Since the value never changes
  I changed it to `const char *` for correctness.
- I verified the PID file path (`./.monitor_pid`) matches the `MONITOR_PID_FILE`
  macro already defined in `city_manager.c` so both programs agree on the location.

#### What I learned

Signal handlers have strict restrictions on which library functions may be called
safely. `printf` is commonly used in teaching examples but is technically unsafe
inside a handler because it can deadlock if the signal interrupts an ongoing `printf`
call in the main thread. The AI did not mention this; I only knew to check because
of prior course material on async-signal safety.

---

### Prompt 5 – add_report helper (city_manager.c)

#### What I asked

> Add a helper function `void add_report(const char *report_id, const char *log_file_path)`
> to `city_manager.c` that:
> 1. Reads the monitor PID from `.monitor_pid`.
> 2. Sends SIGUSR1 to that PID using `kill`.
> 3. Appends a success or failure message to the log file at `log_file_path`.
> If the PID file cannot be opened, or `kill` fails, the log message must explicitly
> say the monitor could not be notified.

#### What was generated

The AI produced a function using `fopen` to read the PID file, `fscanf` to parse the
integer, `kill(monitor_pid, SIGUSR1)` to send the signal, and `fopen`/`fprintf` to
append to the log. It used a `goto log_error` pattern to reach the failure-logging
code from multiple error paths without code duplication.

#### What I changed and why

- The generated function used a local `char log_buf[256]` and `sprintf` to build the
  log line, then wrote it with `fputs`. I kept the `fprintf` style instead, which is
  cleaner and avoids a fixed-size intermediate buffer.
- The AI's error message for a failed `kill` did not include `strerror(errno)`. I
  added it so the log records the actual OS reason (e.g., "No such process") rather
  than a generic failure string, making debugging easier.
- I confirmed that `add_report` is called from `cmd_add` after the record is written
  to disk, passing the string form of `r.id` and the district's `logged_district`
  path — this wiring was not part of the generated code and was added manually.

#### What I learned

The `goto`-for-error-cleanup pattern is idiomatic in C systems code (the Linux kernel
uses it extensively) but AI tools sometimes produce it without explaining the
rationale. Understanding *why* it is used here — to avoid duplicating the log-append
code across several error branches — made it easier to verify the control flow was
correct.

---

## Critical Evaluation

| Aspect | Phase 1 assessment | Phase 2 assessment |
|--------|-------------------|-------------------|
| Correctness of generated logic | Mostly correct for the common cases | Structurally correct; placeholder paths required fixing |
| Type safety | Missed `time_t` width; required manual fix | Const-correctness issues; minor |
| Error handling | Not generated; added manually | Partial; `strerror(errno)` missing from signal-send error |
| Const-correctness | `strtok` on `const char *` was a bug; fixed | `char *` vs `const char *` on PID path; fixed |
| Security / safety | N/A | Pointer comparison for role check was a silent logic bug; fixed |
| Async-signal safety | N/A | `printf` in signal handlers is technically unsafe; noted |
| Code style | Acceptable; minor reformatting | Acceptable; minor reformatting |

Across both phases, the AI was useful for producing working skeletons quickly. However,
every generated block required careful review before integration. The most common
failure modes were: placeholder values that must be adapted to the actual project
layout, missing `#include` directives, and error/edge cases that were omitted entirely.

The bugs that would have been hardest to find at runtime — the pointer comparison for
the role check, the `atoi` on a 64-bit `time_t`, and the hard-coded directory prefix
in `remove_district` — were all caught only by cross-reading the generated code
against the rest of the file and the project specification. None of them would have
produced a compile-time error.

The most valuable part of the exercise remains the review process itself, not the
generated code.
