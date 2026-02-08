/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/sysinfo.c - Display system information dashboard
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <ctype.h>

/* ---- ANSI color helpers ------------------------------------------------- */
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[97m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_RED     "\033[31m"
#define CLR_BCYAN   "\033[1;36m"

#define BOX_W 48

/* ---- Small helpers ------------------------------------------------------ */

static void print_top_border(void) {
    printf(CLR_CYAN "╔");
    for (int i = 0; i < BOX_W; i++) printf("═");
    printf("╗" CLR_RESET "\n");
}

static void print_mid_border(void) {
    printf(CLR_CYAN "╠");
    for (int i = 0; i < BOX_W; i++) printf("═");
    printf("╣" CLR_RESET "\n");
}

static void print_bot_border(void) {
    printf(CLR_CYAN "╚");
    for (int i = 0; i < BOX_W; i++) printf("═");
    printf("╝" CLR_RESET "\n");
}

/*
 * Print a row:  ║  Label     : Value                    ║
 * label_width is the space for the label text (padded).
 */
static void print_row(const char *label, const char *value) {
    /* "  %-10s : %-Ns" where N fills to BOX_W */
    /* inside the box we have BOX_W chars to work with */
    int inner = BOX_W - 2; /* 2 for leading spaces removed below - actually let's just do it */
    printf(CLR_CYAN "║" CLR_RESET "  " CLR_CYAN "%-10s" CLR_RESET " : "
           CLR_WHITE "%-*s" CLR_RESET CLR_CYAN "║" CLR_RESET "\n",
           label, inner - 15, value);
}

/* Print a raw line inside the box (already formatted content) */
static void print_box_line(const char *content) {
    /* We need to pad to BOX_W visible chars.  content may have ANSI codes.
     * Easiest: print content then pad manually. */
    printf(CLR_CYAN "║" CLR_RESET "  %s", content);
    /* We can't easily measure visible width with ANSI, so just pad with
     * enough spaces and close.  We'll use a fixed field approach. */
    printf(CLR_CYAN "║" CLR_RESET "\n");
}

static void print_title(const char *title) {
    int tlen = (int)strlen(title);
    int pad_total = BOX_W - tlen;
    int pad_left = pad_total / 2;
    int pad_right = pad_total - pad_left;
    printf(CLR_CYAN "║" CLR_BOLD CLR_WHITE);
    for (int i = 0; i < pad_left; i++) putchar(' ');
    printf("%s", title);
    for (int i = 0; i < pad_right; i++) putchar(' ');
    printf(CLR_RESET CLR_CYAN "║" CLR_RESET "\n");
}

/* ---- Data readers ------------------------------------------------------- */

static void read_os_name(char *buf, size_t sz) {
    FILE *f = fopen("/etc/os-release", "r");
    buf[0] = '\0';
    if (!f) { snprintf(buf, sz, "Unknown"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            char *start = line + 12;
            /* strip quotes and newline */
            if (*start == '"') start++;
            snprintf(buf, sz, "%s", start);
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '"') buf[len - 1] = '\0';
            break;
        }
    }
    fclose(f);
    if (buf[0] == '\0') snprintf(buf, sz, "Unknown");
}

static void read_uptime(char *buf, size_t sz) {
    FILE *f = fopen("/proc/uptime", "r");
    buf[0] = '\0';
    if (!f) { snprintf(buf, sz, "N/A"); return; }
    double up = 0;
    if (fscanf(f, "%lf", &up) != 1) up = 0;
    fclose(f);
    int total = (int)up;
    int days  = total / 86400;
    int hours = (total % 86400) / 3600;
    int mins  = (total % 3600) / 60;
    int secs  = total % 60;
    if (days > 0)
        snprintf(buf, sz, "%dd %dh %dm %ds", days, hours, mins, secs);
    else if (hours > 0)
        snprintf(buf, sz, "%dh %dm %ds", hours, mins, secs);
    else
        snprintf(buf, sz, "%dm %ds", mins, secs);
}

static void read_cpu(char *name, size_t nsz, int *cores) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    name[0] = '\0';
    *cores = 0;
    if (!f) { snprintf(name, nsz, "N/A"); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "processor", 9) == 0) {
            (*cores)++;
        }
        if (name[0] == '\0' && strncmp(line, "model name", 10) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                strncpy(name, colon, nsz - 1);
                name[nsz - 1] = '\0';
                char *nl = strchr(name, '\n');
                if (nl) *nl = '\0';
            }
        }
    }
    fclose(f);
    if (name[0] == '\0') snprintf(name, nsz, "N/A");
    if (*cores == 0) *cores = 1;
}

static void read_meminfo(unsigned long *mem_total, unsigned long *mem_free,
                         unsigned long *mem_avail,
                         unsigned long *swap_total, unsigned long *swap_free) {
    *mem_total = *mem_free = *mem_avail = 0;
    *swap_total = *swap_free = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long val;
        if (sscanf(line, "MemTotal: %lu kB", &val) == 1)       *mem_total = val;
        else if (sscanf(line, "MemFree: %lu kB", &val) == 1)   *mem_free = val;
        else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1) *mem_avail = val;
        else if (sscanf(line, "SwapTotal: %lu kB", &val) == 1) *swap_total = val;
        else if (sscanf(line, "SwapFree: %lu kB", &val) == 1)  *swap_free = val;
    }
    fclose(f);
}

static void read_loadavg(char *buf, size_t sz) {
    FILE *f = fopen("/proc/loadavg", "r");
    buf[0] = '\0';
    if (!f) { snprintf(buf, sz, "N/A"); return; }
    double l1, l5, l15;
    if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3)
        snprintf(buf, sz, "%.2f %.2f %.2f", l1, l5, l15);
    else
        snprintf(buf, sz, "N/A");
    fclose(f);
}

static int count_processes(void) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Check if directory name is all digits (a PID) */
        const char *p = ent->d_name;
        if (*p == '\0') continue;
        bool numeric = true;
        while (*p) {
            if (!isdigit((unsigned char)*p)) { numeric = false; break; }
            p++;
        }
        if (numeric) count++;
    }
    closedir(d);
    return count;
}

/* ---- Progress bar ------------------------------------------------------- */

static void format_bar(char *buf, size_t sz, int percent, int width) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int filled = (percent * width) / 100;
    int empty  = width - filled;

    const char *color;
    if (percent < 60)       color = CLR_GREEN;
    else if (percent < 85)  color = CLR_YELLOW;
    else                    color = CLR_RED;

    int off = 0;
    off += snprintf(buf + off, sz - off, "%s[", color);
    for (int i = 0; i < filled && off < (int)sz - 1; i++)
        off += snprintf(buf + off, sz - off, "█");
    for (int i = 0; i < empty && off < (int)sz - 1; i++)
        off += snprintf(buf + off, sz - off, "░");
    off += snprintf(buf + off, sz - off, "]" CLR_RESET);
    (void)off;
}

/* ---- Main entry point --------------------------------------------------- */

/*
 * sysinfo
 *
 * Display a colourful system information dashboard.
 */
int builtin_sysinfo(Shell *shell, int argc, char **argv) {
    (void)shell; (void)argc; (void)argv;

    /* Gather data */
    struct utsname uts;
    uname(&uts);

    char os_name[256];
    read_os_name(os_name, sizeof(os_name));

    char uptime[64];
    read_uptime(uptime, sizeof(uptime));

    char cpu_name[128];
    int cpu_cores;
    read_cpu(cpu_name, sizeof(cpu_name), &cpu_cores);

    unsigned long mem_total, mem_free, mem_avail, swap_total, swap_free;
    read_meminfo(&mem_total, &mem_free, &mem_avail, &swap_total, &swap_free);

    char loadavg[64];
    read_loadavg(loadavg, sizeof(loadavg));

    int procs = count_processes();

    /* Disk usage for / */
    struct statvfs vfs;
    double disk_total = 0, disk_used = 0;
    int disk_pct = 0;
    if (statvfs("/", &vfs) == 0) {
        disk_total = (double)vfs.f_blocks * vfs.f_frsize / (1024.0 * 1024 * 1024);
        double disk_free = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024 * 1024);
        disk_used = disk_total - disk_free;
        disk_pct = (disk_total > 0) ? (int)(disk_used * 100.0 / disk_total) : 0;
    }

    /* Memory calculations */
    double mem_total_gib = mem_total / (1024.0 * 1024.0);
    unsigned long mem_used_kb = mem_total - mem_avail;
    double mem_used_gib = mem_used_kb / (1024.0 * 1024.0);
    int mem_pct = (mem_total > 0) ? (int)(mem_used_kb * 100UL / mem_total) : 0;

    double swap_total_gib = swap_total / (1024.0 * 1024.0);
    unsigned long swap_used_kb = swap_total - swap_free;
    double swap_used_gib = swap_used_kb / (1024.0 * 1024.0);
    int swap_pct = (swap_total > 0) ? (int)(swap_used_kb * 100UL / swap_total) : 0;

    /* Format value strings */
    char val[256];

    /* Print dashboard */
    print_top_border();
    print_title("vsh System Information");
    print_mid_border();

    print_row("OS",       os_name);
    print_row("Kernel",   uts.release);
    print_row("Hostname", uts.nodename);
    print_row("Uptime",   uptime);
    print_row("Shell",    "vsh 1.0.0");

    snprintf(val, sizeof(val), "%d", procs);
    print_row("Processes", val);

    print_mid_border();

    /* Truncate CPU name if needed */
    if (strlen(cpu_name) > 30) {
        cpu_name[30] = '\0';
    }
    print_row("CPU",  cpu_name);
    snprintf(val, sizeof(val), "%d", cpu_cores);
    print_row("Cores",    val);
    print_row("Load Avg", loadavg);

    print_mid_border();

    /* Memory with bar */
    snprintf(val, sizeof(val), "%.1f/%.1f GiB (%d%%)",
             mem_used_gib, mem_total_gib, mem_pct);
    print_row("Memory", val);

    char bar[512];
    format_bar(bar, sizeof(bar), mem_pct, 24);
    char bar_line[600];
    /* Pad the bar line to fit inside the box */
    snprintf(bar_line, sizeof(bar_line), "%s%*s", bar, BOX_W - 28, "");
    print_box_line(bar_line);

    /* Swap */
    snprintf(val, sizeof(val), "%.1f/%.1f GiB (%d%%)",
             swap_used_gib, swap_total_gib, swap_pct);
    print_row("Swap", val);

    /* Disk */
    snprintf(val, sizeof(val), "%.1f/%.1f GiB (%d%%)",
             disk_used, disk_total, disk_pct);
    print_row("Disk (/)", val);

    print_bot_border();

    return 0;
}
