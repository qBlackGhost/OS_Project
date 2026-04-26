# AI Usage Documentation – city_manager Phase 1

**Course:** SO/SO1 – Operating Systems  
**Student:** *(fill in your name)*  
**Date:** April 2026  
**Tool used:** Claude (Anthropic) via claude.ai

---

## Overview

As required by the project specification, I used an AI assistant to help implement
two helper functions for the `filter` command:

1. `parse_condition(const char *input, char *field, char *op, char *value)`
2. `match_condition(Report *r, const char *field, const char *op, const char *value)`

All other code in `city_manager.c` was written by me without AI generation.

---

## Prompt 1 – parse_condition

### What I asked

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

### What was generated

The AI produced a function using `strchr` to find the first and second `:` separator,
then copying the substrings into the output buffers using `strncpy` and `strcpy`.
The structure was correct and matched what I needed.

### What I changed and why

- The original version used `strtok`, which modifies the input string. Since I pass
  a `const char *` from `argv`, modifying it would be undefined behaviour. I rewrote
  it to use `strchr` on the original pointer without mutation.
- I added a final validation check: the function now returns 0 if any of the three
  parts is empty (zero length), because conditions like `":==:2"` or `"severity::"` 
  should be rejected early.
- I added a comment explaining *why* we look for exactly two colons, since the operator
  itself (`>=`, `<=`) contains no colons, so the split is unambiguous.

### What I learned

Using `strtok` on a `const char *` is a subtle bug that the compiler does not always
warn about. The AI did not flag this issue — I caught it by reading the code carefully
and thinking about where the string comes from (`argv`).

---

## Prompt 2 – match_condition

### What I asked

> Now generate a function:
> `int match_condition(Report *r, const char *field, const char *op, const char *value);`
> that returns 1 if the report satisfies the condition and 0 otherwise.
> The numeric fields are `severity` (int) and `timestamp` (time_t).
> The string fields are `category` and `inspector` (both char arrays).
> Supported operators: `==`, `!=`, `<`, `<=`, `>`, `>=`.

### What was generated

The AI produced a function that branched on the field name, converted the value string
to the appropriate C type (`atoi` for integers), and compared using the operator string.
The general structure was correct.

### What I changed and why

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

### What I learned

AI-generated comparison functions tend to handle the "happy path" well but skip
error/edge cases (unknown fields, unknown operators, type width issues). Reviewing
the function against the actual struct definition — especially `time_t` being 64-bit —
is essential and cannot be delegated to the AI.

---

## Critical Evaluation

| Aspect | Assessment |
|--------|-----------|
| Correctness of generated logic | Mostly correct for the common cases |
| Type safety | Missed `time_t` width; required manual fix |
| Error handling | Not generated; added manually |
| Const-correctness | `strtok` on `const char *` was a bug; fixed |
| Code style | Acceptable; minor reformatting for consistency |

The AI was useful for producing a working skeleton quickly, but required careful
line-by-line review before integration. The two bugs found (mutable string assumption,
`atoi` on `time_t`) would have caused subtle runtime failures rather than obvious
crashes, making them easy to miss without deliberate review.

The most valuable part of the exercise was the review process itself, not the generated
code.
