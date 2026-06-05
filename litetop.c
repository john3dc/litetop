/*
 * litetop - a minimal htop-like system monitor for the Linux terminal.
 *      Single file, depends only on libc (no ncurses), reads only /proc.
 *
 * Shows CPU (overall + per core), memory + swap, load / uptime / tasks, and a
 * live process list sorted by CPU or memory. Per-process CPU% is computed from
 * the delta of utime+stime between two samples, htop-style (can exceed 100%
 * on multi-core machines, one full core == 100%).
 *
 * Build:  cc -O2 -Wall -o litetop litetop.c
 * Keys:   press ? inside the program, or q to quit.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdarg.h>
#include <poll.h>
#include <pwd.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ------------------------------------------------------------------ */
/* Terminal globals                                                   */
/* ------------------------------------------------------------------ */
static struct termios g_orig, g_raw;
static int g_have_orig = 0, g_in_tui = 0;
static volatile sig_atomic_t g_resized = 1;
static volatile sig_atomic_t g_quit = 0;
static int g_rows = 24, g_cols = 80;

/* ------------------------------------------------------------------ */
/* Safe allocation                                                    */
/* ------------------------------------------------------------------ */
static void leave_tui(void);
static void die(const char *m) { leave_tui(); fprintf(stderr, "litetop: %s\n", m); exit(1); }
static void *xmalloc(size_t n)  { void *p = malloc(n);     if (!p) die("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) die("out of memory"); return q; }

/* ------------------------------------------------------------------ */
/* Dynamic string buffer (one write() per frame)                      */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t len, cap; } Buf;

static void buf_reserve(Buf *b, size_t need) {
    if (b->len + need + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        while (nc < b->len + need + 1) nc *= 2;
        b->p = xrealloc(b->p, nc);
        b->cap = nc;
    }
}
static void buf_add(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void buf_s(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;
    buf_add(b, tmp, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Colours                                                            */
/* ------------------------------------------------------------------ */
#define C_RESET "\x1b[0m"
#define C_DIM   "\x1b[2m"
#define C_HDR   "\x1b[1;36m"
#define C_LBL   "\x1b[38;5;245m"
#define C_GREEN "\x1b[38;5;46m"
#define C_YEL   "\x1b[38;5;226m"
#define C_RED   "\x1b[38;5;196m"
#define C_BLUE  "\x1b[38;5;39m"
#define C_BAR   "\x1b[30;46m"             /* black on cyan: column header (litefm) */
#define C_KEY   "\x1b[1;36m"              /* bold cyan: footer key           */
#define C_KLBL  "\x1b[38;5;250m"          /* light grey: footer label        */
#define C_SEP   "\x1b[38;5;240m"          /* dark grey: footer separators    */
#define VBAR    "\xe2\x94\x82"            /* '│' box-drawing vertical bar     */

/* pick a colour for a load fraction 0..1 */
static const char *load_col(double f) {
    if (f >= 0.85) return C_RED;
    if (f >= 0.50) return C_YEL;
    return C_GREEN;
}

/* ------------------------------------------------------------------ */
/* Terminal setup / teardown                                          */
/* ------------------------------------------------------------------ */
static void wr(const char *s) { ssize_t r = write(STDOUT_FILENO, s, strlen(s)); (void)r; }

static void leave_tui(void) {
    if (!g_in_tui) return;
    if (g_have_orig) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    /* mouse off, cursor on, leave alternate screen */
    wr("\x1b[?1000l\x1b[?1006l\x1b[?25h\x1b[?1049l");
    g_in_tui = 0;
}
static void enter_tui(void) {
    if (g_have_orig) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_raw);
    /* alt screen, cursor off, clear, SGR mouse reporting on */
    wr("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[?1000h\x1b[?1006h");
    g_in_tui = 1;
}

static void on_sig(int s)    { (void)s; g_quit = 1; }
static void on_winch(int s)  { (void)s; g_resized = 1; }

static void get_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        g_rows = ws.ws_row;
        g_cols = ws.ws_col;
    }
    if (g_cols > 1000) g_cols = 1000;
    g_resized = 0;
}

static void setup_term(void) {
    if (!isatty(STDIN_FILENO)) die("not a terminal");
    if (tcgetattr(STDIN_FILENO, &g_orig) == 0) g_have_orig = 1;
    g_raw = g_orig;
    g_raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    g_raw.c_iflag &= ~(IXON | ICRNL);
    g_raw.c_cc[VMIN] = 0;
    g_raw.c_cc[VTIME] = 0;

    struct sigaction sa = {0};
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    struct sigaction sw = {0};
    sw.sa_handler = on_winch;
    sigaction(SIGWINCH, &sw, NULL);

    atexit(leave_tui);
    get_size();
    enter_tui();
}

/* ------------------------------------------------------------------ */
/* Key input (with timeout so the screen can refresh on its own)      */
/* ------------------------------------------------------------------ */
enum {
    K_NONE = -1,
    K_UP = 1000, K_DOWN, K_PGUP, K_PGDN, K_HOME, K_END, K_MOUSE,
    K_ESC, K_ENTER, K_BS, K_F3, K_F6, K_F9, K_F10,
};

/* last decoded mouse event (SGR 1006): button, 1-based column / row, press? */
static int g_mb = 0, g_mx = 0, g_my = 0, g_mpress = 0;

static int read_byte(int ms) {
    struct pollfd pf = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&pf, 1, ms) <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}

/* parse the tail of an SGR mouse report: "<b;x;yM" or "<b;x;ym" (the '<' is
 * already consumed). Returns K_MOUSE on success, 27 on a malformed sequence. */
static int parse_mouse(void) {
    int v[3] = {0,0,0}, idx = 0, ch;
    while ((ch = read_byte(20)) >= 0) {
        if (ch >= '0' && ch <= '9') {
            v[idx] = v[idx] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (++idx > 2) idx = 2;
        } else if (ch == 'M' || ch == 'm') {
            g_mb = v[0]; g_mx = v[1]; g_my = v[2];
            g_mpress = (ch == 'M');
            return K_MOUSE;
        } else {
            break;
        }
    }
    return 27;
}

/* wait up to `ms` for a key; return K_NONE on timeout */
static int read_key(int ms) {
    int c = read_byte(ms);
    if (c < 0) return K_NONE;
    if (c == 127 || c == 8) return K_BS;
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c != 27) return c;

    /* escape sequence; a lone ESC (nothing follows) means cancel */
    int a = read_byte(20);
    if (a < 0) return K_ESC;
    if (a != '[' && a != 'O') return K_ESC;
    int b = read_byte(20);
    switch (b) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case 'R': return K_F3;           /* SS3 F3 (ESC O R) */
        case '<': return parse_mouse();
    }
    /* numeric CSI sequences: "<num>~" — PgUp/PgDn, Home/End, F-keys */
    if (b >= '0' && b <= '9') {
        int num = b - '0', d;
        while ((d = read_byte(20)) >= 0 && d >= '0' && d <= '9')
            num = num * 10 + (d - '0');   /* loop ends on the trailing '~' */
        switch (num) {
            case 1: case 7: return K_HOME;
            case 4: case 8: return K_END;
            case 5:  return K_PGUP;
            case 6:  return K_PGDN;
            case 13: return K_F3;
            case 17: return K_F6;
            case 20: return K_F9;
            case 21: return K_F10;
        }
    }
    return K_ESC;
}

/* ------------------------------------------------------------------ */
/* Small /proc helpers                                                */
/* ------------------------------------------------------------------ */
static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

/* human-readable bytes into a fixed buffer, e.g. "3.41G" */
static void human(unsigned long long kb, char *out, size_t cap) {
    double v = (double)kb;
    const char *u[] = { "K", "M", "G", "T" };
    int i = 0;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    if (i == 0 || v >= 100) snprintf(out, cap, "%.0f%s", v, u[i]); /* whole KB, big values */
    else                    snprintf(out, cap, "%.1f%s", v, u[i]);
}

/* ------------------------------------------------------------------ */
/* CPU stats (aggregate + per core)                                   */
/* ------------------------------------------------------------------ */
#define MAXCPU 256
typedef struct { unsigned long long busy, total; } CpuTimes;

static CpuTimes g_cpu_prev[MAXCPU + 1];   /* [0] = aggregate, [1..] = cores */
static double   g_cpu_pct[MAXCPU + 1];    /* computed usage 0..100 */
static int      g_ncpu = 1;

static void cpu_sample(void) {
    char buf[16384];
    if (read_file("/proc/stat", buf, sizeof buf) < 0) return;
    char *line = buf;
    int idx = 0;          /* 0 = aggregate, then cores in order */
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "cpu", 3) == 0) {
            unsigned long long v[10] = {0};
            const char *p = line + 3;
            while (*p && !isdigit((unsigned char)*p) && *p != ' ') p++;
            int cpunum = (idx == 0) ? -1 : (idx - 1);  /* sanity only */
            (void)cpunum;
            /* skip the "cpuN " label */
            const char *sp = strchr(line, ' ');
            if (sp) {
                int got = sscanf(sp, " %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                                 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
                if (got >= 4 && idx <= MAXCPU) {
                    unsigned long long idle = v[3] + v[4];   /* idle + iowait */
                    unsigned long long total = 0;
                    for (int i = 0; i < 10; i++) total += v[i];
                    unsigned long long busy = total - idle;
                    unsigned long long dt = total - g_cpu_prev[idx].total;
                    unsigned long long db = busy  - g_cpu_prev[idx].busy;
                    g_cpu_pct[idx] = dt ? (100.0 * (double)db / (double)dt) : 0.0;
                    g_cpu_prev[idx].total = total;
                    g_cpu_prev[idx].busy  = busy;
                    if (idx > 0 && idx > g_ncpu) g_ncpu = idx;
                    idx++;
                }
            }
        } else if (idx > 0) {
            break;  /* past the cpu lines */
        }
        line = nl ? nl + 1 : NULL;
    }
    if (idx > 1) g_ncpu = idx - 1;
}

/* total cpu jiffies delta of the aggregate line, for process CPU% */
static unsigned long long g_total_jiffies_prev = 0;
static unsigned long long g_total_jiffies_delta = 1;

/* ------------------------------------------------------------------ */
/* Memory stats                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned long long mem_total, mem_avail, mem_used;
    unsigned long long swap_total, swap_free, swap_used;
} MemInfo;

static void mem_sample(MemInfo *m) {
    memset(m, 0, sizeof *m);
    char buf[8192];
    if (read_file("/proc/meminfo", buf, sizeof buf) < 0) return;
    char *line = buf;
    unsigned long long total = 0, avail = 0, st = 0, sf = 0, val;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if      (sscanf(line, "MemTotal: %llu kB", &val) == 1) total = val;
        else if (sscanf(line, "MemAvailable: %llu kB", &val) == 1) avail = val;
        else if (sscanf(line, "SwapTotal: %llu kB", &val) == 1) st = val;
        else if (sscanf(line, "SwapFree: %llu kB", &val) == 1) sf = val;
        line = nl ? nl + 1 : NULL;
    }
    m->mem_total = total;
    m->mem_avail = avail;
    m->mem_used  = (total > avail) ? total - avail : 0;
    m->swap_total = st;
    m->swap_free  = sf;
    m->swap_used  = (st > sf) ? st - sf : 0;
}

/* ------------------------------------------------------------------ */
/* Process table                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    int pid;
    uid_t uid;
    char user[24];
    char state;
    char comm[64];
    unsigned long long jiffies;     /* utime + stime, current sample  */
    unsigned long long prev_jiffies;
    unsigned long long rss_kb;
    double cpu_pct;
    double mem_pct;
    unsigned long long cputime_sec; /* total cpu seconds used */
} Proc;

static Proc *g_procs = NULL;
static int   g_nproc = 0, g_cap_proc = 0;

static Proc *g_prev = NULL;     /* previous snapshot for delta lookup */
static int   g_nprev = 0;

static unsigned long long find_prev_jiffies(int pid) {
    for (int i = 0; i < g_nprev; i++)
        if (g_prev[i].pid == pid) return g_prev[i].jiffies;
    return (unsigned long long)-1;   /* sentinel: no previous sample */
}

static const char *uid_name(uid_t uid, char *out, size_t cap) {
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) { snprintf(out, cap, "%s", pw->pw_name); }
    else                   { snprintf(out, cap, "%u", (unsigned)uid); }
    return out;
}

static long g_clk_tck = 100;

static void proc_scan(const MemInfo *mem) {
    DIR *d = opendir("/proc");
    if (!d) return;

    /* recompute total jiffies delta from the aggregate cpu line */
    {
        unsigned long long total = g_cpu_prev[0].total;
        g_total_jiffies_delta = (total > g_total_jiffies_prev)
                              ? total - g_total_jiffies_prev : 1;
        g_total_jiffies_prev = total;
    }

    g_nproc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;

        char path[64], buf[4096];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        if (read_file(path, buf, sizeof buf) < 0) continue;

        /* comm is wrapped in (...) and may contain spaces/parens: split on
         * the last ')' so the fixed-position fields after it line up. */
        char *lp = strchr(buf, '(');
        char *rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp) continue;

        if (g_nproc >= g_cap_proc) {
            g_cap_proc = g_cap_proc ? g_cap_proc * 2 : 256;
            g_procs = xrealloc(g_procs, g_cap_proc * sizeof *g_procs);
        }
        Proc *pr = &g_procs[g_nproc];
        memset(pr, 0, sizeof *pr);
        pr->pid = pid;

        size_t clen = (size_t)(rp - lp - 1);
        if (clen >= sizeof pr->comm) clen = sizeof pr->comm - 1;
        memcpy(pr->comm, lp + 1, clen);
        pr->comm[clen] = 0;

        /* fields after ')': state(3) ppid(4) ... utime(14) stime(15) ...
         * field index 1 = pid, 2 = comm. After rp we are at field 3. */
        char state = '?';
        unsigned long long utime = 0, stime = 0, rss_pages = 0;
        const char *p = rp + 2;       /* skip ") " */
        /* field 3 */
        state = *p;
        /* advance through fields 3..24; we need 14,15 (utime,stime) and 24 (rss) */
        int field = 3;
        while (*p && field < 24) {
            if (*p == ' ') {
                field++;
                p++;
                if (field == 14) utime = strtoull(p, NULL, 10);
                else if (field == 15) stime = strtoull(p, NULL, 10);
                else if (field == 24) rss_pages = strtoull(p, NULL, 10);
                continue;
            }
            p++;
        }
        pr->state = state;
        pr->jiffies = utime + stime;
        pr->rss_kb = rss_pages * (unsigned long long)(sysconf(_SC_PAGESIZE) / 1024);
        pr->cputime_sec = pr->jiffies / (unsigned long long)g_clk_tck;

        /* owner uid from the stat dir */
        struct stat stt;
        snprintf(path, sizeof path, "/proc/%d", pid);
        pr->uid = (stat(path, &stt) == 0) ? stt.st_uid : 0;
        uid_name(pr->uid, pr->user, sizeof pr->user);

        /* CPU% from delta vs previous snapshot */
        unsigned long long prevj = find_prev_jiffies(pid);
        if (prevj != (unsigned long long)-1 && pr->jiffies >= prevj) {
            unsigned long long dj = pr->jiffies - prevj;
            pr->cpu_pct = 100.0 * (double)dj * (double)g_ncpu / (double)g_total_jiffies_delta;
        } else {
            pr->cpu_pct = 0.0;
        }
        pr->prev_jiffies = prevj;

        pr->mem_pct = mem->mem_total ? 100.0 * (double)pr->rss_kb / (double)mem->mem_total : 0.0;

        g_nproc++;
    }
    closedir(d);

    /* swap snapshots so the next scan can diff against this one */
    free(g_prev);
    g_prev = xmalloc(g_nproc * sizeof *g_prev);
    memcpy(g_prev, g_procs, g_nproc * sizeof *g_prev);
    g_nprev = g_nproc;
}

/* ------------------------------------------------------------------ */
/* Sorting                                                            */
/* ------------------------------------------------------------------ */
enum { SORT_CPU, SORT_MEM, SORT_PID, SORT_APP, SORT_TIME, SORT_RSS };
static int g_sort = SORT_CPU;

static int cmp_proc(const void *a, const void *b) {
    const Proc *x = a, *y = b;
    if (g_sort == SORT_CPU) {
        if (x->cpu_pct < y->cpu_pct) return 1;
        if (x->cpu_pct > y->cpu_pct) return -1;
    } else if (g_sort == SORT_MEM) {
        if (x->rss_kb < y->rss_kb) return 1;
        if (x->rss_kb > y->rss_kb) return -1;
    } else if (g_sort == SORT_APP) {
        int r = strcasecmp(x->comm, y->comm);   /* alphabetical by app name */
        if (r) return r;
    } else if (g_sort == SORT_TIME) {
        if (x->cputime_sec < y->cputime_sec) return 1;
        if (x->cputime_sec > y->cputime_sec) return -1;
    } else if (g_sort == SORT_RSS) {
        if (x->rss_kb < y->rss_kb) return 1;
        if (x->rss_kb > y->rss_kb) return -1;
    }
    return x->pid - y->pid;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

/* Draw a labelled bar occupying exactly `width` terminal cells, htop-style:
 *
 *     LBL[||||||           52.0%]
 *
 * Layout per cell: a 4-char right-padded label, '[', `inner` cells, ']'.
 * The suffix text is overlaid RIGHT-ALIGNED *inside* the bar, so the whole
 * thing never grows past `width` and two bars sit side by side cleanly. */
static void draw_bar(Buf *o, const char *label, double frac, int width,
                     const char *suffix) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int inner = width - 3 /*label*/ - 2 /*brackets*/;
    if (inner < 1) inner = 1;
    int filled = (int)(frac * inner + 0.5);
    if (filled > inner) filled = inner;

    int slen = suffix ? (int)strlen(suffix) : 0;
    if (slen > inner) slen = inner;
    int sstart = inner - slen;          /* first cell covered by the suffix */

    buf_printf(o, "%s%3.3s%s[", C_LBL, label, C_RESET);

    const char *cur = "";               /* current SGR, to avoid re-emitting */
    for (int i = 0; i < inner; i++) {
        const char *col;
        char ch;
        if (i >= sstart) {              /* suffix region: bright text overlay */
            col = "\x1b[1m";
            ch  = suffix[i - sstart];
        } else if (i < filled) {        /* filled part of the bar */
            col = load_col(frac);
            ch  = '|';
        } else {                        /* empty track */
            col = C_DIM;
            ch  = ' ';
        }
        if (col != cur) { buf_s(o, col); cur = col; }
        buf_add(o, &ch, 1);
    }
    buf_printf(o, "%s]", C_RESET);
}

static unsigned long long g_uptime_sec = 0;
static double g_load[3] = {0,0,0};

static void read_uptime_load(void) {
    char buf[256];
    if (read_file("/proc/uptime", buf, sizeof buf) >= 0)
        g_uptime_sec = (unsigned long long)strtod(buf, NULL);
    if (read_file("/proc/loadavg", buf, sizeof buf) >= 0)
        sscanf(buf, "%lf %lf %lf", &g_load[0], &g_load[1], &g_load[2]);
}

static int g_scroll = 0;     /* index of the first visible row (into view)  */
static int g_cursor = 0;     /* index of the selected row (into view)       */
static int g_list_row0 = 0;  /* screen row (1-based) of the first proc line  */
static int g_list_avail = 1; /* number of visible process rows              */

/* The "view" is the filtered, sorted subset of g_procs that is actually on
 * screen. It holds indices into g_procs; the cursor/scroll index into it. */
static int  *g_view = NULL;
static int   g_nview = 0, g_view_cap = 0;
static char  g_filter[64] = "";   /* active search/filter substring         */

/* does a process match the active filter? (case-insensitive, comm or user) */
static int proc_matches(const Proc *p) {
    if (!g_filter[0]) return 1;
    return strcasestr(p->comm, g_filter) != NULL ||
           strcasestr(p->user, g_filter) != NULL;
}

/* rebuild g_view from the (already sorted) g_procs, applying the filter */
static void build_view(void) {
    g_nview = 0;
    for (int i = 0; i < g_nproc; i++) {
        if (!proc_matches(&g_procs[i])) continue;
        if (g_nview >= g_view_cap) {
            g_view_cap = g_view_cap ? g_view_cap * 2 : 256;
            g_view = xrealloc(g_view, g_view_cap * sizeof *g_view);
        }
        g_view[g_nview++] = i;
    }
}

/* selected process, or NULL if the view is empty */
static Proc *sel_proc(void) {
    if (g_cursor < 0 || g_cursor >= g_nview) return NULL;
    return &g_procs[g_view[g_cursor]];
}

/* The selection is tied to the row position, not to a process: across a
 * refresh or re-sort the cursor stays on the same index and simply points
 * at whatever process now occupies that row. So this just clamps. */
static void clamp_cursor(void) {
    if (g_cursor >= g_nview) g_cursor = g_nview - 1;
    if (g_cursor < 0) g_cursor = 0;
}

/* move the selection by `delta` rows */
static void move_cursor(int delta) {
    g_cursor += delta;
    clamp_cursor();
}

/* a one-shot status line shown in the footer until the next full refresh */
static char g_msg[160];

/* the latest memory sample, so overlays can repaint the background by
 * calling render() without threading the value through every call */
static MemInfo g_mem;

static void render(const MemInfo *mem) {
    Buf o = {0};
    buf_s(&o, "\x1b[H");   /* home, then overdraw each line + clear-to-eol */

    char tbuf[16];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", lt);

    int W = g_cols;
    int line = 1;

    /* ---- header, htop-style: a column of meters on the left, a small block
     * of system stats on the right. The meter column is the left half on a
     * wide terminal; on a narrow one the stats drop below the meters. ---- */
    char suf[32], lbl[16];
    char ub[16], tb[16];

    int two_col  = (W >= 70);
    int meterw   = two_col ? W / 2 : W;     /* visible width of each meter */
    int rightcol = meterw + 3;              /* 1-based column for stats text */

    int n_meters = g_ncpu + 2;              /* cores + Mem + Swp */

    /* count running tasks for the stats block */
    int running = 0;
    for (int i = 0; i < g_nproc; i++) if (g_procs[i].state == 'R') running++;

    /* left column: per-core CPU meters */
    for (int c = 1; c <= g_ncpu; c++) {
        snprintf(lbl, sizeof lbl, "%d", c - 1);
        snprintf(suf, sizeof suf, "%.1f%%", g_cpu_pct[c]);
        draw_bar(&o, lbl, g_cpu_pct[c] / 100.0, meterw, suf);
        buf_s(&o, "\x1b[K\r\n"); line++;
    }
    /* Mem meter */
    human(mem->mem_used, ub, sizeof ub);
    human(mem->mem_total, tb, sizeof tb);
    snprintf(suf, sizeof suf, "%s/%s", ub, tb);
    draw_bar(&o, "Mem", mem->mem_total ? (double)mem->mem_used / (double)mem->mem_total : 0,
             meterw, suf);
    buf_s(&o, "\x1b[K\r\n"); line++;
    /* Swp meter (always shown, htop-style, even when there is no swap) */
    human(mem->swap_used, ub, sizeof ub);
    human(mem->swap_total, tb, sizeof tb);
    snprintf(suf, sizeof suf, "%s/%s", ub, tb);
    draw_bar(&o, "Swp", mem->swap_total ? (double)mem->swap_used / (double)mem->swap_total : 0,
             meterw, suf);
    buf_s(&o, "\x1b[K\r\n"); line++;

    /* uptime split into d / h / m / s */
    unsigned long long up = g_uptime_sec;
    int upd = (int)(up / 86400); up %= 86400;
    int uph = (int)(up / 3600);  up %= 3600;
    int upm = (int)(up / 60);    int ups = (int)(up % 60);
    int ncpu = g_ncpu > 0 ? g_ncpu : 1;
    const char *lc = load_col(g_load[0] / ncpu);

    /* build the right-hand stats lines */
    char stat[4][96];
    int nstat = 0;
    if (g_filter[0])
        snprintf(stat[nstat++], sizeof stat[0],
                 "%sTasks:%s %s%d%s, %s%d shown%s",
                 C_LBL, C_RESET, C_HDR, g_nproc, C_RESET, C_BLUE, g_nview, C_RESET);
    else
        snprintf(stat[nstat++], sizeof stat[0],
                 "%sTasks:%s %s%d%s, %s%d running%s",
                 C_LBL, C_RESET, C_HDR, g_nproc, C_RESET,
                 running ? C_GREEN : C_DIM, running, C_RESET);
    snprintf(stat[nstat++], sizeof stat[0],
             "%sLoad average:%s %s%.2f%s %.2f %.2f",
             C_LBL, C_RESET, lc, g_load[0], C_RESET, g_load[1], g_load[2]);
    if (upd) snprintf(stat[nstat++], sizeof stat[0],
             "%sUptime:%s %s%dd %02d:%02d:%02d%s",
             C_LBL, C_RESET, C_HDR, upd, uph, upm, ups, C_RESET);
    else     snprintf(stat[nstat++], sizeof stat[0],
             "%sUptime:%s %s%02d:%02d:%02d%s",
             C_LBL, C_RESET, C_HDR, uph, upm, ups, C_RESET);
    snprintf(stat[nstat++], sizeof stat[0],
             "%sTime:%s %s%s%s", C_LBL, C_RESET, C_HDR, tbuf, C_RESET);

    if (two_col) {
        /* the header is as tall as the taller of the two columns */
        int hdr_rows = n_meters > nstat ? n_meters : nstat;
        /* clear rows that the stats use but no meter reached yet */
        for (; line <= hdr_rows; line++)
            buf_printf(&o, "\x1b[%d;1H\x1b[K", line);
        /* overlay each stats line on its row via absolute positioning */
        for (int i = 0; i < nstat; i++)
            buf_printf(&o, "\x1b[%d;%dH%s", 1 + i, rightcol, stat[i]);
        buf_printf(&o, "\x1b[%d;1H", line);     /* park cursor below the block */
    } else {
        /* narrow: stats stacked under the meters, one per line */
        for (int i = 0; i < nstat; i++) {
            buf_printf(&o, "%s\x1b[K\r\n", stat[i]); line++;
        }
    }

    /* blank spacer line between the meter block and the process table */
    buf_s(&o, "\x1b[0m\x1b[K\r\n"); line++;

    /* column header: a green bar, htop-style; sorted column marked with a ▼ */
    #define DN "\xe2\x96\xbc"
    const char *a_pid = g_sort == SORT_PID ? DN : " ";
    const char *a_cpu = g_sort == SORT_CPU ? DN : " ";
    const char *a_mem = g_sort == SORT_MEM ? DN : " ";
    const char *a_app = g_sort == SORT_APP ? DN : " ";
    buf_printf(&o, "%s %5s%s %-9s %5s%s %5s%s %7s %8s  %s%s",
               C_BAR, "PID", a_pid, "USER", "CPU%", a_cpu, "MEM%", a_mem,
               "RSS", "TIME+", "Application", a_app);
    #undef DN
    if (g_filter[0]) buf_printf(&o, "   search: %s", g_filter);
    buf_s(&o, "\x1b[K");      /* extend the bar to the end of the line */
    buf_s(&o, C_RESET);
    buf_s(&o, "\r\n"); line++;

    /* process rows */
    int header_lines = line - 1;
    int avail = g_rows - header_lines - 1;   /* leave last row for footer */
    if (avail < 1) avail = 1;
    g_list_row0  = line;     /* screen row of the first process line  */
    g_list_avail = avail;

    /* scroll so the cursor stays on screen, then clamp to valid range */
    if (g_cursor < g_scroll)           g_scroll = g_cursor;
    if (g_cursor >= g_scroll + avail)  g_scroll = g_cursor - avail + 1;
    if (g_scroll > g_nview - avail)    g_scroll = g_nview - avail;
    if (g_scroll < 0)                  g_scroll = 0;

    for (int i = 0; i < avail; i++) {
        int idx = g_scroll + i;
        buf_s(&o, "\x1b[0m");
        if (idx >= g_nview) { buf_s(&o, "\x1b[K\r\n"); continue; }
        Proc *p = &g_procs[g_view[idx]];

        unsigned long long cs = p->cputime_sec;
        int cm = (int)(cs / 60), css = (int)(cs % 60);
        char rssb[16];
        human(p->rss_kb, rssb, sizeof rssb);

        int sel = (idx == g_cursor);
        /* on the selected row everything is drawn in reverse video, so we
         * suppress the per-field colours and the inline resets that would
         * otherwise break the highlight. */
        const char *RST = sel ? "" : C_RESET;
        const char *cc  = sel ? "" : load_col(p->cpu_pct / 100.0);
        const char *mc  = sel ? "" : load_col(p->mem_pct / 100.0);
        char user[10];
        snprintf(user, sizeof user, "%-9.9s", p->user);

        /* command, truncated to remaining width */
        int used = 1 + 6 + 1 + 9 + 1 + 6 + 1 + 6 + 1 + 7 + 1 + 8 + 2;
        int cmdw = W - used;
        if (cmdw < 3) cmdw = 3;

        if (sel) buf_s(&o, "\x1b[7m");
        buf_printf(&o, " %6d %s %s%5.1f%s   %s%5.1f%s   %7s %4d:%02d  ",
                   p->pid, user, cc, p->cpu_pct, RST,
                   mc, p->mem_pct, RST, rssb, cm, css);
        /* command */
        if (!sel) {
            if (p->state == 'R') buf_s(&o, C_GREEN);
            else if (p->state == 'Z') buf_s(&o, C_RED);
        }
        for (int k = 0; p->comm[k] && k < cmdw; k++) {
            char ch = p->comm[k];
            buf_add(&o, isprint((unsigned char)ch) ? &ch : (char[]){'?'}, 1);
        }
        if (sel) buf_s(&o, "\x1b[K\x1b[0m");   /* extend highlight to EOL */
        else     buf_s(&o, C_RESET "\x1b[K");
        buf_s(&o, "\r\n");
    }

    /* footer, litefm-style: bold-cyan keys + grey labels, ' | ' separators,
     * Quit pinned to the right. A status message (e.g. after a kill) takes
     * over the whole line for one refresh. The sort key string lower-cases
     * the inactive options and upper-cases the active one. */
    buf_printf(&o, "\x1b[%d;1H\x1b[0m", g_rows);
    if (g_msg[0]) {
        buf_printf(&o, " %s%s%s", C_KLBL, g_msg, C_RESET);
    } else {
        const struct { const char *key, *lbl; } kb[] = {
            { "F3/f", "Search" },
            { "F6/s", "Sort" },
            { "F9/k", "Kill" },
        };
        int col = 0;
        /* keep one cell free at the right edge so the last glyph is never
         * dropped by the terminal's auto-margin -> Quit stays "Quit". */
        const int rightw = 12;             /* "│ F10/q Quit" visible width */
        const int endw   = g_cols - 1;
        buf_s(&o, " "); col += 1;
        for (size_t i = 0; i < sizeof kb / sizeof kb[0]; i++) {
            if (i) { buf_printf(&o, "%s " VBAR " %s", C_SEP, C_RESET); col += 3; }
            buf_printf(&o, "%s%s%s %s%s%s",
                       C_KEY, kb[i].key, C_RESET, C_KLBL, kb[i].lbl, C_RESET);
            col += (int)strlen(kb[i].key) + 1 + (int)strlen(kb[i].lbl);
        }
        /* pad out to the right, then the pinned Quit (one cell shy of the edge) */
        while (col < endw - rightw) { buf_s(&o, " "); col++; }
        buf_printf(&o, "%s" VBAR " %s%sF10/q%s %sQuit%s",
                   C_SEP, C_RESET, C_KEY, C_RESET, C_KLBL, C_RESET);
    }
    buf_s(&o, "\x1b[K\x1b[0m");

    ssize_t r = write(STDOUT_FILENO, o.p, o.len); (void)r;
    free(o.p);
}

/* ------------------------------------------------------------------ */
/* Overlays (centred boxes, litefe-style)                             */
/* ------------------------------------------------------------------ */
static void flush_buf(Buf *o) {
    ssize_t w = write(STDOUT_FILENO, o->p, o->len); (void)w;
    free(o->p); o->p = NULL; o->len = o->cap = 0;
}

/* draw a centred rounded box; report its top-left cell in *bx,*by */
static void overlay_frame(Buf *o, int bw, int bh, int *bx, int *by) {
    if (bw > g_cols) bw = g_cols;
    if (bh > g_rows) bh = g_rows;
    int x = (g_cols - bw) / 2 + 1, y = (g_rows - bh) / 2 + 1;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    buf_printf(o, "\x1b[%d;%dH\x1b[0m\xe2\x95\xad", y, x);          /* ╭ */
    for (int i = 0; i < bw - 2; i++) buf_s(o, "\xe2\x94\x80");      /* ─ */
    buf_s(o, "\xe2\x95\xae");                                       /* ╮ */
    for (int r = 1; r < bh - 1; r++) {
        buf_printf(o, "\x1b[%d;%dH\xe2\x94\x82", y + r, x);         /* │ */
        for (int i = 0; i < bw - 2; i++) buf_s(o, " ");
        buf_s(o, "\xe2\x94\x82");
    }
    buf_printf(o, "\x1b[%d;%dH\xe2\x95\xb0", y + bh - 1, x);        /* ╰ */
    for (int i = 0; i < bw - 2; i++) buf_s(o, "\xe2\x94\x80");
    buf_s(o, "\xe2\x95\xaf");                                       /* ╯ */
    *bx = x; *by = y;
}

/* F3 / f : an overlay that edits g_filter, filtering the list live underneath */
static void search_overlay(void) {
    size_t len = strlen(g_filter);
    int bw = 56, bh = 6, redraw = 1;
    for (;;) {
        if (redraw) {
            build_view();
            clamp_cursor();
            render(&g_mem);                       /* background, live-filtered */
            Buf o = {0};
            int bx, by;
            overlay_frame(&o, bw, bh, &bx, &by);
            int ix = bx + 2;
            buf_printf(&o, "\x1b[%d;%dH%sSearch%s", by + 1, ix, C_HDR, C_RESET);
            buf_printf(&o, "\x1b[%d;%dH%s> %s%s", by + 3, ix, C_KEY, C_RESET, g_filter);
            buf_s(&o, "\x1b[7m \x1b[0m");          /* cursor block */
            buf_printf(&o, "\x1b[%d;%dH%sEnter keep   Esc clear   %d matches%s",
                       by + 4, ix, C_DIM, g_nview, C_RESET);
            flush_buf(&o);
            redraw = 0;
        }
        int k = read_key(300);
        if (k == K_NONE) continue;
        if (k == K_ENTER) return;                  /* keep the filter */
        if (k == K_ESC)   { g_filter[0] = 0; return; }   /* clear & close */
        if (k == K_BS)    { if (len) g_filter[--len] = 0; redraw = 1; }
        else if (k >= 32 && k < 127) {
            if (len + 1 < sizeof g_filter) { g_filter[len++] = (char)k; g_filter[len] = 0; }
            redraw = 1;
        }
    }
}

/* F6 / s : an overlay to pick the sort column; returns 1 if it changed */
static int sort_overlay(void) {
    static const struct { const char *name; int id; } cols[] = {
        { "CPU%",        SORT_CPU  },
        { "MEM%",        SORT_MEM  },
        { "TIME+",       SORT_TIME },
        { "RSS",         SORT_RSS  },
        { "PID",         SORT_PID  },
        { "Application", SORT_APP  },
    };
    int n = (int)(sizeof cols / sizeof cols[0]);
    int sel = 0;
    for (int i = 0; i < n; i++) if (cols[i].id == g_sort) sel = i;
    int bw = 32, bh = n + 4;
    int by = (g_rows - bh) / 2 + 1; if (by < 1) by = 1;
    int redraw = 1;
    for (;;) {
        if (redraw) {
            render(&g_mem);
            Buf o = {0};
            int bx, byy;
            overlay_frame(&o, bw, bh, &bx, &byy);
            int ix = bx + 2;
            buf_printf(&o, "\x1b[%d;%dH%sSort by%s", byy + 1, ix, C_HDR, C_RESET);
            for (int i = 0; i < n; i++) {
                buf_printf(&o, "\x1b[%d;%dH", byy + 2 + i, ix);
                if (i == sel) buf_printf(&o, "\x1b[7m %-*s\x1b[0m", bw - 5, cols[i].name);
                else          buf_printf(&o, " %-*s", bw - 5, cols[i].name);
            }
            flush_buf(&o);
            redraw = 0;
        }
        int k = read_key(300);
        if (k == K_NONE) continue;
        if (k == K_ESC) return 0;
        if (k == K_ENTER) { g_sort = cols[sel].id; return 1; }
        if (k == K_UP)   { if (sel > 0) sel--; redraw = 1; }
        else if (k == K_DOWN) { if (sel < n - 1) sel++; redraw = 1; }
        else if (k == K_MOUSE && g_mpress && g_mb == 0) {
            int row = g_my - (by + 2);
            if (row >= 0 && row < n) { g_sort = cols[row].id; return 1; }
        }
    }
}

/* F9 / k : pick a signal to send to the selected process (htop-style) */
static void kill_overlay(void) {
    Proc *p = sel_proc();
    if (!p) return;
    int pid = p->pid;
    char name[64];
    snprintf(name, sizeof name, "%s", p->comm);

    static const struct { const char *name; int sig; } sigs[] = {
        { "SIGTERM", SIGTERM }, { "SIGKILL", SIGKILL }, { "SIGHUP",  SIGHUP  },
        { "SIGINT",  SIGINT  }, { "SIGQUIT", SIGQUIT }, { "SIGSTOP", SIGSTOP },
        { "SIGCONT", SIGCONT }, { "SIGUSR1", SIGUSR1 }, { "SIGUSR2", SIGUSR2 },
    };
    int n = (int)(sizeof sigs / sizeof sigs[0]);
    int sel = 0;                       /* default: SIGTERM */
    int bw = 40, bh = n + 4;
    int by = (g_rows - bh) / 2 + 1; if (by < 1) by = 1;
    int redraw = 1;
    for (;;) {
        if (redraw) {
            render(&g_mem);
            Buf o = {0};
            int bx, byy;
            overlay_frame(&o, bw, bh, &bx, &byy);
            int ix = bx + 2;
            buf_printf(&o, "\x1b[%d;%dH%sSignal \xe2\x86\x92 %d (%.20s)%s",
                       byy + 1, ix, C_HDR, pid, name, C_RESET);
            for (int i = 0; i < n; i++) {
                buf_printf(&o, "\x1b[%d;%dH", byy + 2 + i, ix);
                if (i == sel) buf_printf(&o, "\x1b[7m %-10s %2d \x1b[0m", sigs[i].name, sigs[i].sig);
                else          buf_printf(&o, " %-10s %s%2d%s", sigs[i].name, C_DIM, sigs[i].sig, C_RESET);
            }
            flush_buf(&o);
            redraw = 0;
        }
        int k = read_key(300);
        if (k == K_NONE) continue;
        int chosen = -1;
        if (k == K_ESC) return;
        else if (k == K_ENTER) chosen = sel;
        else if (k == K_UP)   { if (sel > 0) sel--; redraw = 1; }
        else if (k == K_DOWN) { if (sel < n - 1) sel++; redraw = 1; }
        else if (k == K_MOUSE && g_mpress && g_mb == 0) {
            int row = g_my - (by + 2);
            if (row >= 0 && row < n) chosen = row;
        }
        if (chosen >= 0) {
            if (kill(pid, sigs[chosen].sig) == 0)
                snprintf(g_msg, sizeof g_msg, "Sent %s to %d (%s)", sigs[chosen].name, pid, name);
            else
                snprintf(g_msg, sizeof g_msg, "%s %d: %s", sigs[chosen].name, pid, strerror(errno));
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                          */
/* ------------------------------------------------------------------ */

/* milliseconds elapsed since `t0` on the monotonic clock */
static long ms_since(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

/* act on one key/mouse event; returns 1 if the list must be re-sorted */
static int apply_key(int k) {
    switch (k) {
        case 'q': case 'Q': case 3: case K_F10: g_quit = 1; return 0;
        case 'c': case 'C': g_sort = SORT_CPU; return 1;
        case 'm': case 'M': g_sort = SORT_MEM; return 1;
        case 'p': case 'P': g_sort = SORT_PID; return 1;
        case 'a': case 'A': g_sort = SORT_APP; return 1;
        case K_UP:   move_cursor(-1); return 0;
        case K_DOWN: move_cursor(+1); return 0;
        case K_PGUP: move_cursor(-g_list_avail); return 0;
        case K_PGDN: move_cursor(+g_list_avail); return 0;
        case K_HOME: move_cursor(-g_nproc);      return 0;
        case K_END:  move_cursor(+g_nproc);      return 0;
        case K_MOUSE:
            if (!g_mpress) return 0;                   /* act on press only */
            if (g_mb == 64) move_cursor(-3);           /* wheel up   */
            else if (g_mb == 65) move_cursor(+3);      /* wheel down */
            else if (g_mb == 0) {                      /* left click */
                if (g_my == g_list_row0 - 1) {         /* column header */
                    if      (g_mx <= 8)                g_sort = SORT_PID;
                    else if (g_mx >= 19 && g_mx <= 25) g_sort = SORT_CPU;
                    else if (g_mx >= 26 && g_mx <= 32) g_sort = SORT_MEM;
                    else if (g_mx >= 49)               g_sort = SORT_APP;
                    return 1;
                } else if (g_my >= g_list_row0 &&
                           g_my <  g_list_row0 + g_list_avail) {
                    int idx = g_scroll + (g_my - g_list_row0);
                    if (idx < g_nview) g_cursor = idx;
                }
            }
            return 0;
    }
    return 0;
}

int main(void) {
    g_clk_tck = sysconf(_SC_CLK_TCK);
    if (g_clk_tck <= 0) g_clk_tck = 100;

    setup_term();

    int delay_ms = 1500;

    /* prime the CPU + process deltas, then settle briefly so the FIRST frame
     * already shows meaningful percentages -- and appears fast, not after a
     * full interval. */
    cpu_sample();
    mem_sample(&g_mem);
    proc_scan(&g_mem);
    read_key(250);

    while (!g_quit) {
        if (g_resized) { get_size(); wr("\x1b[2J"); }

        /* one full sample of everything per interval (this is the costly part:
         * it reads every /proc/<pid>/stat), then paint. */
        cpu_sample();
        mem_sample(&g_mem);
        read_uptime_load();
        proc_scan(&g_mem);
        qsort(g_procs, g_nproc, sizeof *g_procs, cmp_proc);
        build_view();                 /* apply the active search filter */
        clamp_cursor();               /* selection stays on its row index */
        g_msg[0] = 0;                 /* clear any one-shot status message */
        render(&g_mem);

        /* Until the interval elapses, react to input only -- no re-scan. Time
         * is measured on the monotonic clock so a burst of held-key autorepeat
         * can never push the next costly scan forward. Queued keys are drained
         * and collapsed into a single repaint. */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!g_quit && !g_resized) {
            long remain = delay_ms - ms_since(&t0);
            if (remain <= 0) break;
            int k = read_key(remain < 120 ? (int)remain : 120);
            if (k == K_NONE) continue;

            /* modal overlays handle their own input + redraw loop */
            if (k == K_F3 || k == 'f' || k == 'F') {
                search_overlay();
                build_view(); clamp_cursor(); render(&g_mem);
                continue;
            }
            if (k == K_F6 || k == 's' || k == 'S') {
                if (sort_overlay()) qsort(g_procs, g_nproc, sizeof *g_procs, cmp_proc);
                build_view(); clamp_cursor(); render(&g_mem);
                continue;
            }
            if (k == K_F9 || k == 'k' || k == 'K') {
                kill_overlay();
                build_view(); clamp_cursor(); render(&g_mem);
                continue;
            }

            int resort = 0;
            do { resort |= apply_key(k); k = read_key(0); }  /* drain the queue */
            while (k != K_NONE && !g_quit);
            if (resort) { qsort(g_procs, g_nproc, sizeof *g_procs, cmp_proc); build_view(); }
            clamp_cursor();
            render(&g_mem);   /* one repaint for the whole batch of input */
        }
    }

    leave_tui();
    free(g_procs);
    free(g_prev);
    free(g_view);
    return 0;
}
