/*
 * city_manager.c
 * SO/OS Project – Phase 1
 *
 * Build: gcc city_manager.c -o city_manager
 * Usage: ./city_manager --role <manager|inspector> --user <name> --<command> [args...]
 */

#include <stdio.h>
#include <stdlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096  // Common value for Linux and macOS
#endif
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>

/* ─────────────────────────────────────────────
 * REPORT STRUCT  –  must be exactly 208 bytes
 * ─────────────────────────────────────────────
 *   int        id          4
 *   char       inspector  32
 *   float      lat         4
 *   float      lon         4
 *   char       category   32
 *   int        severity    4
 *   time_t     timestamp   8   (64-bit systems)
 *   char       description 120
 *                        ───
 *                        208  ✓
 */
typedef struct {
    int    id;               /*   4 bytes  */
    char   inspector[32];    /*  32 bytes  */
    float  lat;              /*   4 bytes  */
    float  lon;              /*   4 bytes  */
    char   category[32];     /*  32 bytes  */
    int    severity;         /*   4 bytes  */
    time_t timestamp;        /*   8 bytes  */
    char   description[120]; /* 120 bytes  */
} Report;                    /* = 208 bytes total */

/* Compile-time guard – breaks compilation if struct size is wrong */
typedef char _assert_report_size[ (sizeof(Report) == 208) ? 1 : -1 ];

/* ─────────────────────────────────────────────
 * CONSTANTS
 * ───────────────────────────────────────────── */
#define ROLE_MANAGER   "manager"
#define ROLE_INSPECTOR "inspector"

#define PERM_DIR       0750   /* rwxr-x--- */
#define PERM_REPORTS   0664   /* rw-rw-r-- */
#define PERM_CFG       0640   /* rw-r----- */
#define PERM_LOG       0644   /* rw-r--r-- */

/* Forward declaration – defined after cmd_filter */
void check_symlink(const char *district);

/* ─────────────────────────────────────────────
 * HELPER: convert permission bits → "rwxrwxrwx" string
 * You must write this yourself per the spec.
 * ───────────────────────────────────────────── */
void mode_to_string(mode_t mode, char out[10]) {
    out[0] = (mode & S_IRUSR) ? 'r' : '-';
    out[1] = (mode & S_IWUSR) ? 'w' : '-';
    out[2] = (mode & S_IXUSR) ? 'x' : '-';
    out[3] = (mode & S_IRGRP) ? 'r' : '-';
    out[4] = (mode & S_IWGRP) ? 'w' : '-';
    out[5] = (mode & S_IXGRP) ? 'x' : '-';
    out[6] = (mode & S_IROTH) ? 'r' : '-';
    out[7] = (mode & S_IWOTH) ? 'w' : '-';
    out[8] = (mode & S_IXOTH) ? 'x' : '-';
    out[9] = '\0';
}

/* ─────────────────────────────────────────────
 * HELPER: check that a file has EXACTLY the expected permission bits
 * Returns 1 if OK, 0 if mismatch (also prints a diagnostic).
 * ───────────────────────────────────────────── */
int check_permissions(const char *path, mode_t expected) {
    struct stat st;
    if (stat(path, &st) < 0) {
        perror(path);
        return 0;
    }
    mode_t actual = st.st_mode & 0777;
    if (actual != expected) {
        char exp_str[10], act_str[10];
        mode_to_string(expected, exp_str);
        mode_to_string(actual,   act_str);
        fprintf(stderr,
            "ERROR: %s has permissions %s but expected %s\n",
            path, act_str, exp_str);
        return 0;
    }
    return 1;
}

/* ─────────────────────────────────────────────
 * HELPER: log an action to logged_district
 * Format: "<timestamp>\t<user>\t<role>\t<action>\n"
 *
 * logged_district has mode 644: owner (manager) may write,
 * group/other may only read.  Per spec we check the permission
 * bits with stat() and refuse if the declared role would not
 * have write access under the scheme (only owner-write bit set).
 * ───────────────────────────────────────────── */
void log_action(const char *district, const char *user,
                const char *role, const char *action) {
    char path[256];
    snprintf(path, sizeof(path), "%s/logged_district", district);

    /* Create file if it does not exist yet */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, PERM_LOG);
    if (fd < 0) { perror("log open"); return; }
    close(fd);
    chmod(path, PERM_LOG);

    /* Check permission bits with stat() as required by spec.
     * logged_district is 644: only owner-write (S_IWUSR) is set.
     * Inspectors map to the group role → no write bit → refuse. */
    struct stat st;
    if (stat(path, &st) < 0) { perror("log stat"); return; }

    if (strcmp(role, ROLE_INSPECTOR) == 0) {
        /* Group-write bit NOT set on 644 → inspector cannot write */
        if (!(st.st_mode & S_IWGRP)) {
            fprintf(stderr,
                "ERROR: permission denied – inspector role cannot write "
                "to logged_district (mode %o lacks group-write)\n",
                (unsigned)(st.st_mode & 0777));
            return;
        }
    }

    fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) { perror("log open"); return; }

    time_t now = time(NULL);
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "%ld\t%s\t%s\t%s\n",
                       (long)now, user, role, action);
    write(fd, buf, len);
    close(fd);
}

/* ─────────────────────────────────────────────
 * HELPER: create (or verify) a district directory and its files
 * ───────────────────────────────────────────── */
void ensure_district(const char *district) {
    struct stat st;

    /* Create directory if missing */
    if (stat(district, &st) < 0) {
        if (mkdir(district, PERM_DIR) < 0) {
            perror("mkdir"); exit(1);
        }
        chmod(district, PERM_DIR);
    }

    /* Create district.cfg if missing */
    char cfg[256];
    snprintf(cfg, sizeof(cfg), "%s/district.cfg", district);
    if (stat(cfg, &st) < 0) {
        int fd = open(cfg, O_WRONLY | O_CREAT, PERM_CFG);
        if (fd < 0) { perror("cfg create"); exit(1); }
        /* Default severity threshold = 1 */
        write(fd, "threshold=1\n", 12);
        close(fd);
        chmod(cfg, PERM_CFG);
    }

    /* Create reports.dat if missing */
    char rpt[256];
    snprintf(rpt, sizeof(rpt), "%s/reports.dat", district);
    if (stat(rpt, &st) < 0) {
        int fd = open(rpt, O_WRONLY | O_CREAT, PERM_REPORTS);
        if (fd < 0) { perror("reports.dat create"); exit(1); }
        close(fd);
        chmod(rpt, PERM_REPORTS);
    }

    /* Create active_reports-<district> symlink in CWD */
    char link_name[256];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);

    char target[256];
    snprintf(target, sizeof(target), "%s/reports.dat", district);

    /* lstat so we detect existing symlinks without following them */
    if (lstat(link_name, &st) < 0) {
        if (symlink(target, link_name) < 0) {
            perror("symlink");
        }
    }

    /* Always verify the symlink is not dangling */
    check_symlink(district);
}

/* ─────────────────────────────────────────────
 * HELPER: count records in reports.dat
 * ───────────────────────────────────────────── */
int count_reports(const char *district) {
    char path[256];
    snprintf(path, sizeof(path), "%s/reports.dat", district);
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (int)(st.st_size / sizeof(Report));
}

/* ─────────────────────────────────────────────
 * COMMAND: add
 * Both roles may add. Prompts user for fields interactively.
 * ───────────────────────────────────────────── */
void cmd_add(const char *district, const char *user, const char *role) {
    ensure_district(district);

    char rpt_path[256];
    snprintf(rpt_path, sizeof(rpt_path), "%s/reports.dat", district);

    /* Both roles may write to reports.dat (mode 664) – just verify */
    if (!check_permissions(rpt_path, PERM_REPORTS)) {
        /* Auto-fix: set them back to spec */
        chmod(rpt_path, PERM_REPORTS);
    }

    Report r;
    memset(&r, 0, sizeof(Report));

    /* Assign next ID */
    r.id = count_reports(district) + 1;

    /* Inspector name comes from --user */
    strncpy(r.inspector, user, sizeof(r.inspector) - 1);

    /* Prompt for remaining fields */
    printf("X: "); scanf("%f", &r.lat);
    printf("Y: "); scanf("%f", &r.lon);

    printf("Category (road/lighting/flooding/other): ");
    scanf("%31s", r.category);

    printf("Severity level (1/2/3): ");
    scanf("%d", &r.severity);
    if (r.severity < 1 || r.severity > 3) {
        fprintf(stderr, "ERROR: severity must be 1, 2, or 3\n");
        exit(1);
    }

    printf("Description: ");
    /* consume leftover newline from scanf */
    int ch; while ((ch = getchar()) != '\n' && ch != EOF);
    fgets(r.description, sizeof(r.description), stdin);
    /* strip trailing newline */
    r.description[strcspn(r.description, "\n")] = '\0';

    r.timestamp = time(NULL);

    /* Append record */
    int fd = open(rpt_path, O_WRONLY | O_APPEND);
    if (fd < 0) { perror("reports.dat open"); exit(1); }
    if (write(fd, &r, sizeof(Report)) != sizeof(Report)) {
        perror("write"); close(fd); exit(1);
    }
    close(fd);

    printf("Report #%d added to district '%s'.\n", r.id, district);
    log_action(district, user, role, "add");
}

/* ─────────────────────────────────────────────
 * HELPER: print a single report
 * ───────────────────────────────────────────── */
void print_report(const Report *r) {
    char time_buf[64];
    struct tm *tm_info = localtime(&r->timestamp);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("  ID:          %d\n",   r->id);
    printf("  Inspector:   %s\n",   r->inspector);
    printf("  GPS:         (%.4f, %.4f)\n", r->lat, r->lon);
    printf("  Category:    %s\n",   r->category);
    printf("  Severity:    %d\n",   r->severity);
    printf("  Timestamp:   %s\n",   time_buf);
    printf("  Description: %s\n",   r->description);
}

/* ─────────────────────────────────────────────
 * COMMAND: list
 * Both roles. Prints permission bits, size, mtime of reports.dat.
 * Uses lstat() on reports.dat so a symlink is identified as such,
 * not silently followed (spec requirement).
 * ───────────────────────────────────────────── */
void cmd_list(const char *district) {
    char rpt_path[256];
    snprintf(rpt_path, sizeof(rpt_path), "%s/reports.dat", district);

    struct stat st;
    /* lstat: if reports.dat is itself a symlink, we see the link, not the target */
    if (lstat(rpt_path, &st) < 0) {
        fprintf(stderr, "ERROR: district '%s' not found or no reports.dat\n", district);
        exit(1);
    }

    /* If it is a symlink, warn and follow to get real size */
    if (S_ISLNK(st.st_mode)) {
        fprintf(stderr, "WARNING: reports.dat is a symlink – following to read\n");
        if (stat(rpt_path, &st) < 0) {
            fprintf(stderr, "ERROR: dangling symlink for reports.dat\n");
            exit(1);
        }
    }

    /* Permission string */
    char perm_str[10];
    mode_to_string(st.st_mode & 0777, perm_str);

    /* Last modification time */
    char mtime_buf[64];
    struct tm *tm_info = localtime(&st.st_mtime);
    strftime(mtime_buf, sizeof(mtime_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("reports.dat  %s  %lld bytes  %s\n",
           perm_str, (long long)st.st_size, mtime_buf);

    int n = (int)(st.st_size / sizeof(Report));
    printf("%d report(s) in district '%s':\n\n", n, district);

    if (n == 0) return;

    int fd = open(rpt_path, O_RDONLY);
    if (fd < 0) { perror("open"); exit(1); }

    Report r;
    for (int i = 0; i < n; i++) {
        if (read(fd, &r, sizeof(Report)) != sizeof(Report)) {
            perror("read"); close(fd); exit(1);
        }
        printf("--- Report %d ---\n", i + 1);
        print_report(&r);
        printf("\n");
    }
    close(fd);
}

/* ─────────────────────────────────────────────
 * COMMAND: view
 * Both roles. Print one report by ID.
 * ───────────────────────────────────────────── */
void cmd_view(const char *district, int report_id) {
    char rpt_path[256];
    snprintf(rpt_path, sizeof(rpt_path), "%s/reports.dat", district);

    struct stat st;
    if (lstat(rpt_path, &st) < 0) {
        fprintf(stderr, "ERROR: district '%s' does not exist or has no reports.dat\n",
                district);
        exit(1);
    }

    int fd = open(rpt_path, O_RDONLY);
    if (fd < 0) { perror("open"); exit(1); }

    Report r;
    int found = 0;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        if (r.id == report_id) {
            print_report(&r);
            found = 1;
            break;
        }
    }
    close(fd);

    if (!found)
        fprintf(stderr, "ERROR: report #%d not found in district '%s'\n",
                report_id, district);
}

/* ─────────────────────────────────────────────
 * COMMAND: remove_report
 * Manager only. Shifts records left, truncates file.
 * ───────────────────────────────────────────── */
void cmd_remove_report(const char *district, const char *role, int report_id,
                       const char *user) {
    if (strcmp(role, ROLE_MANAGER) != 0) {
        fprintf(stderr, "ERROR: only manager role can remove reports\n");
        exit(1);
    }

    char rpt_path[256];
    snprintf(rpt_path, sizeof(rpt_path), "%s/reports.dat", district);

    int fd = open(rpt_path, O_RDWR);
    if (fd < 0) { perror("open"); exit(1); }

    struct stat st;
    fstat(fd, &st);
    int n = (int)(st.st_size / sizeof(Report));

    /* Find the index of the record to delete */
    int del_idx = -1;
    Report r;
    for (int i = 0; i < n; i++) {
        lseek(fd, (off_t)i * sizeof(Report), SEEK_SET);
        read(fd, &r, sizeof(Report));
        if (r.id == report_id) { del_idx = i; break; }
    }

    if (del_idx < 0) {
        fprintf(stderr, "ERROR: report #%d not found\n", report_id);
        close(fd); exit(1);
    }

    /* Shift every record after del_idx one position left */
    for (int i = del_idx + 1; i < n; i++) {
        lseek(fd, (off_t)i * sizeof(Report), SEEK_SET);
        read(fd, &r, sizeof(Report));
        lseek(fd, (off_t)(i - 1) * sizeof(Report), SEEK_SET);
        write(fd, &r, sizeof(Report));
    }

    /* Truncate the last (now-duplicate) record */
    ftruncate(fd, (off_t)(n - 1) * sizeof(Report));
    close(fd);

    printf("Report #%d removed from district '%s'.\n", report_id, district);
    log_action(district, user, role, "remove_report");
}

/* ─────────────────────────────────────────────
 * COMMAND: update_threshold
 * Manager only. Verifies district.cfg has permissions 640 before writing.
 * ───────────────────────────────────────────── */
void cmd_update_threshold(const char *district, const char *role,
                          int value, const char *user) {
    if (strcmp(role, ROLE_MANAGER) != 0) {
        fprintf(stderr, "ERROR: only manager role can update threshold\n");
        exit(1);
    }

    char cfg_path[256];
    snprintf(cfg_path, sizeof(cfg_path), "%s/district.cfg", district);

    /* Spec: verify bits are EXACTLY 640 before writing */
    if (!check_permissions(cfg_path, PERM_CFG)) {
        fprintf(stderr,
            "ERROR: district.cfg permissions have been altered – refusing to write\n");
        exit(1);
    }

    int fd = open(cfg_path, O_WRONLY | O_TRUNC);
    if (fd < 0) { perror("cfg open"); exit(1); }

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "threshold=%d\n", value);
    write(fd, buf, len);
    close(fd);

    printf("Threshold updated to %d in district '%s'.\n", value, district);
    log_action(district, user, role, "update_threshold");
}

/* ─────────────────────────────────────────────
 * FILTER FUNCTIONS  (AI-assisted per spec)
 *
 * These two functions were generated with AI assistance.
 * See ai_usage.md for prompts, output, and changes made.
 * ───────────────────────────────────────────── */

/*
 * parse_condition – split "field:op:value" into three parts.
 * Returns 1 on success, 0 on failure.
 */
int parse_condition(const char *input, char *field, char *op, char *value) {
    /* We need to find exactly two ':' separators.
     * The operator itself may contain a colon (e.g. "<="), but by the spec
     * format field:operator:value the FIRST colon ends the field,
     * the SECOND colon ends the operator. */
    const char *p1 = strchr(input, ':');
    if (!p1) return 0;

    const char *p2 = strchr(p1 + 1, ':');
    if (!p2) return 0;

    /* field */
    int field_len = (int)(p1 - input);
    strncpy(field, input, field_len);
    field[field_len] = '\0';

    /* operator */
    int op_len = (int)(p2 - p1 - 1);
    strncpy(op, p1 + 1, op_len);
    op[op_len] = '\0';

    /* value */
    strcpy(value, p2 + 1);

    return (field_len > 0 && op_len > 0 && strlen(value) > 0) ? 1 : 0;
}

/*
 * match_condition – return 1 if report r satisfies field op value.
 * Numeric fields: severity, timestamp.  String fields: category, inspector.
 */
int match_condition(Report *r, const char *field, const char *op, const char *value) {
    /* ── String fields ── */
    if (strcmp(field, "category") == 0 || strcmp(field, "inspector") == 0) {
        const char *actual = (strcmp(field, "category") == 0)
                             ? r->category : r->inspector;
        int cmp = strcmp(actual, value);
        if (strcmp(op, "==") == 0) return cmp == 0;
        if (strcmp(op, "!=") == 0) return cmp != 0;
        /* <, <=, >, >= also work lexicographically */
        if (strcmp(op, "<")  == 0) return cmp <  0;
        if (strcmp(op, "<=") == 0) return cmp <= 0;
        if (strcmp(op, ">")  == 0) return cmp >  0;
        if (strcmp(op, ">=") == 0) return cmp >= 0;
        return 0;
    }

    /* ── Numeric fields ── */
    long actual = 0;
    if (strcmp(field, "severity") == 0)
        actual = (long)r->severity;
    else if (strcmp(field, "timestamp") == 0)
        actual = (long)r->timestamp;
    else {
        fprintf(stderr, "WARNING: unknown filter field '%s'\n", field);
        return 0;
    }

    long target = atol(value);
    if (strcmp(op, "==") == 0) return actual == target;
    if (strcmp(op, "!=") == 0) return actual != target;
    if (strcmp(op, "<")  == 0) return actual <  target;
    if (strcmp(op, "<=") == 0) return actual <= target;
    if (strcmp(op, ">")  == 0) return actual >  target;
    if (strcmp(op, ">=") == 0) return actual >= target;

    fprintf(stderr, "WARNING: unknown operator '%s'\n", op);
    return 0;
}

/* ─────────────────────────────────────────────
 * COMMAND: filter
 * ───────────────────────────────────────────── */
void cmd_filter(const char *district, char **conditions, int num_conditions) {
    char rpt_path[256];
    snprintf(rpt_path, sizeof(rpt_path), "%s/reports.dat", district);

    struct stat st;
    if (lstat(rpt_path, &st) < 0) {
        fprintf(stderr, "ERROR: district '%s' does not exist or has no reports.dat\n",
                district);
        exit(1);
    }

    int fd = open(rpt_path, O_RDONLY);
    if (fd < 0) { perror("open"); exit(1); }

    /* Parse all conditions up front */
    char fields[16][64], ops[16][8], values[16][128];
    for (int i = 0; i < num_conditions; i++) {
        if (!parse_condition(conditions[i], fields[i], ops[i], values[i])) {
            fprintf(stderr, "ERROR: bad condition '%s'\n", conditions[i]);
            close(fd); exit(1);
        }
    }

    Report r;
    int matches = 0;
    while (read(fd, &r, sizeof(Report)) == sizeof(Report)) {
        int ok = 1;
        for (int i = 0; i < num_conditions; i++) {
            if (!match_condition(&r, fields[i], ops[i], values[i])) {
                ok = 0; break;
            }
        }
        if (ok) {
            print_report(&r);
            printf("\n");
            matches++;
        }
    }
    close(fd);

    printf("%d report(s) matched.\n", matches);
}

/* ─────────────────────────────────────────────
 * COMMAND: check_symlinks  (utility – not in spec but useful)
 * Uses lstat() to detect dangling symlinks.
 * ───────────────────────────────────────────── */
void check_symlink(const char *district) {
    char link_name[256];
    snprintf(link_name, sizeof(link_name), "active_reports-%s", district);

    struct stat lst, st;
    if (lstat(link_name, &lst) < 0) {
        fprintf(stderr, "INFO: no symlink '%s' found\n", link_name);
        return;
    }

    if (!S_ISLNK(lst.st_mode)) {
        fprintf(stderr, "WARNING: '%s' is not a symlink\n", link_name);
        return;
    }

    /* Try following – if it fails the link is dangling */
    if (stat(link_name, &st) < 0) {
        fprintf(stderr, "WARNING: symlink '%s' is dangling (target missing)\n",
                link_name);
    } else {
        printf("Symlink '%s' → OK (%lld bytes)\n",
               link_name, (long long)st.st_size);
    }
}

/* ─────────────────────────────────────────────
 * MAIN – argument parsing
 * ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Verify struct size at runtime too */
    if (sizeof(Report) != 208) {
        fprintf(stderr, "FATAL: sizeof(Report) = %zu, expected 208\n",
                sizeof(Report));
        return 1;
    }

    /* ── Parse named arguments ── */
    const char *role   = NULL;
    const char *user   = NULL;
    const char *cmd    = NULL;   /* command name without "--" */

    /* We'll collect positional arguments (district, report_id, etc.)
     * and extra conditions for filter */
    char *pos[32];
    int   npos = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            role = argv[++i];
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            user = argv[++i];
        } else if (strncmp(argv[i], "--", 2) == 0) {
            /* Any other --flag is the command */
            cmd = argv[i] + 2;   /* skip the "--" */
        } else {
            /* Positional: district, report_id, conditions, threshold value */
            pos[npos++] = argv[i];
        }
    }

    /* Validate required globals */
    if (!role) { fprintf(stderr, "ERROR: --role is required\n"); return 1; }
    if (!cmd)  { fprintf(stderr, "ERROR: no command given\n");   return 1; }
    if (!user && strcmp(cmd, "list") != 0 && strcmp(cmd, "view") != 0
              && strcmp(cmd, "filter") != 0) {
        fprintf(stderr, "ERROR: --user is required for command '%s'\n", cmd);
        return 1;
    }

    /* Validate role */
    if (strcmp(role, ROLE_MANAGER) != 0 && strcmp(role, ROLE_INSPECTOR) != 0) {
        fprintf(stderr, "ERROR: role must be 'manager' or 'inspector'\n");
        return 1;
    }

    /* ── Dispatch ── */
    if (strcmp(cmd, "add") == 0) {
        if (npos < 1) { fprintf(stderr, "Usage: --add <district>\n"); return 1; }
        cmd_add(pos[0], user, role);

    } else if (strcmp(cmd, "list") == 0) {
        if (npos < 1) { fprintf(stderr, "Usage: --list <district>\n"); return 1; }
        cmd_list(pos[0]);

    } else if (strcmp(cmd, "view") == 0) {
        if (npos < 2) { fprintf(stderr, "Usage: --view <district> <id>\n"); return 1; }
        cmd_view(pos[0], atoi(pos[1]));

    } else if (strcmp(cmd, "remove_report") == 0) {
        if (npos < 2) {
            fprintf(stderr, "Usage: --remove_report <district> <id>\n"); return 1;
        }
        cmd_remove_report(pos[0], role, atoi(pos[1]), user);

    } else if (strcmp(cmd, "update_threshold") == 0) {
        if (npos < 2) {
            fprintf(stderr, "Usage: --update_threshold <district> <value>\n"); return 1;
        }
        cmd_update_threshold(pos[0], role, atoi(pos[1]), user);

    } else if (strcmp(cmd, "remove_district") == 0) {
        if (role != ROLE_MANAGER) {
            fprintf(stderr, "ERROR: Only managers can remove districts.\n");
            return 1;
        }
        if (npos < 1) {
            fprintf(stderr, "Usage: --remove_district <district_id>\n");
            return 1;
        }

        const char *district_id = pos[0];
        char district_dir[PATH_MAX];
        char symlink_path[PATH_MAX];

        // Construct the district directory path
        snprintf(district_dir, sizeof(district_dir), "/path/to/districts/%s", district_id);
        snprintf(symlink_path, sizeof(symlink_path), "/path/to/active_reports-%s", district_id);

        // Validate the district directory path
        if (access(district_dir, F_OK) != 0) {
            fprintf(stderr, "ERROR: District directory '%s' does not exist.\n", district_dir);
            return 1;
        }

        // Remove the district directory and symlink
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            execlp("rm", "rm", "-rf", district_dir, NULL);
            // If execlp fails
            perror("ERROR: Failed to execute rm");
            exit(1);
        } else if (pid > 0) {
            // Parent process
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                // Remove the symlink
                if (unlink(symlink_path) != 0) {
                    perror("ERROR: Failed to remove symlink");
                    return 1;
                }
                printf("District '%s' and its contents were successfully removed.\n", district_id);
            } else {
                fprintf(stderr, "ERROR: Failed to remove district directory '%s'.\n", district_dir);
                return 1;
            }
        } else {
            // Fork failed
            perror("ERROR: Failed to fork process");
            return 1;
        }
    } else if (strcmp(cmd, "filter") == 0) {
        if (npos < 2) {
            fprintf(stderr,
                "Usage: --filter <district> <field:op:value> ...\n"); return 1;
        }
        /* pos[0] = district, pos[1..] = conditions */
        cmd_filter(pos[0], pos + 1, npos - 1);

    } else {
        fprintf(stderr, "ERROR: unknown command '%s'\n", cmd);
        return 1;
    }

    return 0;
}