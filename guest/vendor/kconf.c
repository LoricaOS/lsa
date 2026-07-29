/*
 * kconf — a small Kconfig engine for LoricaOS.
 *
 * Parses Kconfig files, resolves symbol values (depends/default/select) into a
 * .config, and generates the two artifacts a build consumes:
 *   - autoconf.h : #define CONFIG_FOO 1        (for source #ifdef guards)
 *   - auto.conf  : CONFIG_FOO=y                (a Make snippet for obj gating)
 *
 * Scope: bool symbols only (LoricaOS/Aegis has no loadable modules, so there is
 * no tristate). Unsupported constructs fail loudly rather than silently doing
 * the wrong thing — a miscompiled kernel config is worse than a hard error.
 *
 * This is deliberately one translation unit: it builds with a plain C compiler
 * and no deps, so it can eventually run on LoricaOS itself.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- util ------------------------------------------------------------- */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("kconf: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) die("out of memory");
    return p;
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) die("out of memory");
    return p;
}

/* ---- expressions ------------------------------------------------------ */

typedef enum { E_SYM, E_NOT, E_AND, E_OR, E_EQ, E_NEQ } etype;

struct symbol;

typedef struct expr {
    etype t;
    struct expr *l, *r;   /* operands (l only, for E_NOT) */
    struct symbol *sym;   /* E_SYM / E_EQ / E_NEQ left-hand symbol */
    char *val;            /* E_EQ / E_NEQ right-hand literal ("y"/"n"/...) */
} expr;

/* ---- symbols ---------------------------------------------------------- */

typedef struct deflt {
    int val;              /* default value (0/1) */
    expr *cond;           /* NULL == unconditional */
    struct deflt *next;
} deflt;

typedef struct sel {
    struct symbol *target;
    expr *cond;           /* NULL == unconditional */
    struct sel *next;
} sel;

typedef struct symbol {
    char *name;
    int has_prompt;       /* user-selectable (had a `bool "prompt"`) */
    int is_bool;          /* a bool symbol (value is 0/1) */
    int is_int;           /* an int symbol (value is the number itself) */
    expr *dep;            /* effective `depends on` (incl. menu/if context) */
    deflt *defaults;
    sel *selects;

    int value;            /* resolved 0/1 */
    int prev;             /* value at the start of the current resolve pass */
    int user_set;         /* set from .config / defconfig / all{no,yes} */
    int user_value;
    int in_order;         /* appended to the ordered list yet */
    struct symbol *next;      /* hash chain */
    struct symbol *order_next; /* definition order, for stable .config output */
} symbol;

#define HASH 1021
static symbol *symtab[HASH];
static symbol *order_head, *order_tail;

static unsigned hash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h % HASH;
}

static symbol *sym_find(const char *name) {
    for (symbol *s = symtab[hash(name)]; s; s = s->next)
        if (!strcmp(s->name, name)) return s;
    return NULL;
}

static symbol *sym_get(const char *name) {
    symbol *s = sym_find(name);
    if (s) return s;
    s = xmalloc(sizeof *s);
    s->name = xstrdup(name);
    unsigned h = hash(name);
    s->next = symtab[h];
    symtab[h] = s;
    if (!order_head) order_head = order_tail = s;
    else { order_tail->order_next = s; order_tail = s; }
    s->in_order = 1;
    return s;
}

/* ---- expression evaluation ------------------------------------------- */

static int expr_eval(expr *e) {
    if (!e) return 1; /* absent condition == true */
    switch (e->t) {
    case E_SYM:  return e->sym->value != 0;
    case E_NOT:  return !expr_eval(e->l);
    case E_AND:  return expr_eval(e->l) && expr_eval(e->r);
    case E_OR:   return expr_eval(e->l) || expr_eval(e->r);
    case E_EQ:
    case E_NEQ: {
        int lv = e->sym->value != 0;
        int rv = (!strcmp(e->val, "y")) ? 1 : (!strcmp(e->val, "n")) ? 0 : -1;
        int eq;
        if (rv < 0) {                 /* symbol=symbol: compare by name symbol */
            symbol *r = sym_get(e->val);
            eq = (lv == (r->value != 0));
        } else {
            eq = (lv == rv);
        }
        return e->t == E_EQ ? eq : !eq;
    }
    }
    return 0;
}

/* ---- expression parser (precedence: || < && < ! < primary) ----------- */

typedef struct { const char *p; } lexer;

static void lskip(lexer *L) { while (*L->p == ' ' || *L->p == '\t') L->p++; }

/* read an identifier or a "quoted"/y/n literal into buf; returns 1 if any */
static int ltoken(lexer *L, char *buf, size_t n) {
    lskip(L);
    size_t i = 0;
    if (*L->p == '"') {
        L->p++;
        while (*L->p && *L->p != '"' && i < n - 1) buf[i++] = *L->p++;
        if (*L->p == '"') L->p++;
        buf[i] = 0;
        return 1;
    }
    while ((isalnum((unsigned char)*L->p) || *L->p == '_') && i < n - 1)
        buf[i++] = *L->p++;
    buf[i] = 0;
    return i > 0;
}

static expr *parse_or(lexer *L);

static expr *mkexpr(etype t) { expr *e = xmalloc(sizeof *e); e->t = t; return e; }

static expr *parse_primary(lexer *L) {
    lskip(L);
    if (*L->p == '(') {
        L->p++;
        expr *e = parse_or(L);
        lskip(L);
        if (*L->p == ')') L->p++;
        return e;
    }
    if (*L->p == '!') {
        L->p++;
        expr *e = mkexpr(E_NOT);
        e->l = parse_primary(L);
        return e;
    }
    char buf[256];
    if (!ltoken(L, buf, sizeof buf)) return NULL;
    symbol *s = sym_get(buf);
    lskip(L);
    if ((L->p[0] == '=' ) || (L->p[0] == '!' && L->p[1] == '=')) {
        etype t = (*L->p == '=') ? E_EQ : E_NEQ;
        L->p += (t == E_EQ) ? 1 : 2;
        char rhs[256];
        ltoken(L, rhs, sizeof rhs);
        expr *e = mkexpr(t);
        e->sym = s;
        e->val = xstrdup(rhs);
        return e;
    }
    expr *e = mkexpr(E_SYM);
    e->sym = s;
    return e;
}

static expr *parse_and(lexer *L) {
    expr *e = parse_primary(L);
    for (;;) {
        lskip(L);
        if (L->p[0] == '&' && L->p[1] == '&') L->p += 2;
        else break;
        expr *r = parse_primary(L);
        expr *a = mkexpr(E_AND);
        a->l = e; a->r = r; e = a;
    }
    return e;
}

static expr *parse_or(lexer *L) {
    expr *e = parse_and(L);
    for (;;) {
        lskip(L);
        if (L->p[0] == '|' && L->p[1] == '|') L->p += 2;
        else break;
        expr *r = parse_and(L);
        expr *o = mkexpr(E_OR);
        o->l = e; o->r = r; e = o;
    }
    return e;
}

static expr *parse_expr(const char *s) {
    lexer L = { s };
    return parse_or(&L);
}

static expr *expr_and(expr *a, expr *b) {
    if (!a) return b;
    if (!b) return a;
    expr *e = mkexpr(E_AND);
    e->l = a; e->r = b;
    return e;
}

/* ---- Kconfig file parser --------------------------------------------- */

/* context-dependency stack: `if EXPR` and `menu ... depends on EXPR` push a
 * dependency that is AND-ed into every symbol defined inside. */
#define CTXMAX 64
static expr *ctx[CTXMAX];
static int ctxn;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

/* does `line` start with keyword `kw` followed by space/end? sets *rest */
static int kw(char *line, const char *k, char **rest) {
    size_t n = strlen(k);
    if (strncmp(line, k, n)) return 0;
    if (line[n] && line[n] != ' ' && line[n] != '\t') return 0;
    *rest = trim(line + n);
    return 1;
}

static void parse_file(const char *path);

static void parse_stream(FILE *f, const char *path) {
    char raw[4096];
    symbol *cur = NULL;    /* symbol currently being defined */
    int in_help = 0;
    (void)path;

    while (fgets(raw, sizeof raw, f)) {
        if (in_help) {
            /* help block ends at the first non-blank line indented less than
             * the help body. We keep it simple: help ends at a line with text
             * in column 0 (a new top-level construct). Blank lines stay. */
            char *t = raw;
            if (*t == '\n' || *t == '\r') continue;
            if (*t == ' ' || *t == '\t') continue; /* still help body */
            in_help = 0; /* fallthrough to reparse this line */
        }
        char *line = trim(raw);
        if (!*line || *line == '#') continue;

        char *rest;
        if (kw(line, "config", &rest) || kw(line, "menuconfig", &rest)) {
            cur = sym_get(rest);
            cur->dep = NULL;
            /* inherit active context deps */
            for (int i = 0; i < ctxn; i++) cur->dep = expr_and(cur->dep, ctx[i]);
            continue;
        }
        if (kw(line, "bool", &rest)) {
            if (!cur) die("bool outside config");
            cur->is_bool = 1;
            if (*rest) cur->has_prompt = 1; /* had a "prompt" string */
            /* a trailing `if EXPR` on the prompt gates visibility */
            char *ifp = strstr(rest, " if ");
            if (ifp) cur->dep = expr_and(cur->dep, parse_expr(ifp + 4));
            continue;
        }
        if (kw(line, "int", &rest) || kw(line, "hex", &rest)) {
            if (!cur) die("int outside config");
            cur->is_int = 1;
            if (*rest) cur->has_prompt = 1; /* had a "prompt" string */
            continue;
        }
        if (kw(line, "tristate", &rest) || kw(line, "string", &rest)) {
            die("unsupported symbol type near '%s' (bool/int only)", line);
        }
        if (kw(line, "depends on", &rest)) {
            if (!cur) die("depends outside config");
            cur->dep = expr_and(cur->dep, parse_expr(rest));
            continue;
        }
        if (kw(line, "default", &rest)) {
            if (!cur) die("default outside config");
            /* `default <val> [if <cond>]` */
            char *ifp = strstr(rest, " if ");
            expr *cond = NULL;
            if (ifp) { cond = parse_expr(ifp + 4); *ifp = 0; }
            char *v = trim(rest);
            deflt *d = xmalloc(sizeof *d);
            if (cur->is_int) {
                d->val = (int)strtol(v, NULL, 0);   /* numeric default (0x.. ok) */
                d->cond = cond;
            } else if (!strcmp(v, "y") || !strcmp(v, "n")) {
                d->val = (v[0] == 'y');
                d->cond = cond;
            } else {
                /* `default SYM` — a bool that is y when SYM is y */
                d->val = 1;
                d->cond = expr_and(cond, parse_expr(v));
            }
            d->next = cur->defaults;
            cur->defaults = d;
            continue;
        }
        if (kw(line, "select", &rest)) {
            if (!cur) die("select outside config");
            char *ifp = strstr(rest, " if ");
            expr *cond = NULL;
            if (ifp) { cond = parse_expr(ifp + 4); *ifp = 0; }
            sel *s = xmalloc(sizeof *s);
            s->target = sym_get(trim(rest));
            s->cond = cond;
            s->next = cur->selects;
            cur->selects = s;
            continue;
        }
        if (kw(line, "help", &rest) || kw(line, "---help---", &rest)) {
            in_help = 1;
            continue;
        }
        if (kw(line, "if", &rest)) {
            if (ctxn >= CTXMAX) die("if nesting too deep");
            ctx[ctxn++] = parse_expr(rest);
            cur = NULL;
            continue;
        }
        if (kw(line, "endif", &rest)) {
            if (ctxn > 0) ctxn--;
            cur = NULL;
            continue;
        }
        if (kw(line, "menu", &rest)) {
            /* push a placeholder; a following `depends on` (handled via the
             * menu's own config-less lines) is rare — we push NULL and let any
             * `visible if`/`depends on` on the menu attach to nothing for MVP */
            if (ctxn >= CTXMAX) die("menu nesting too deep");
            ctx[ctxn++] = NULL;
            cur = NULL;
            continue;
        }
        if (kw(line, "endmenu", &rest)) {
            if (ctxn > 0) ctxn--;
            cur = NULL;
            continue;
        }
        if (kw(line, "source", &rest)) {
            char buf[1024];
            lexer L = { rest };
            ltoken(&L, buf, sizeof buf);
            parse_file(buf);
            continue;
        }
        if (kw(line, "comment", &rest) || kw(line, "mainmenu", &rest) ||
            kw(line, "prompt", &rest) || kw(line, "range", &rest) ||
            kw(line, "visible", &rest) || kw(line, "option", &rest)) {
            continue; /* cosmetic / unsupported-but-harmless */
        }
        if (kw(line, "choice", &rest) || kw(line, "endchoice", &rest)) {
            die("choice groups not supported yet: '%s'", line);
        }
        die("unrecognized Kconfig line: '%s'", line);
    }
}

static void parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open Kconfig '%s'", path);
    int saved = ctxn; /* source'd file must balance its own if/menu */
    parse_stream(f, path);
    ctxn = saved;
    fclose(f);
}

/* ---- resolution ------------------------------------------------------- */

/* first default whose condition holds, else 0 */
static int sym_default(symbol *s) {
    /* defaults were prepended, so reverse to honor first-wins ordering */
    int val = 0, found = 0;
    /* collect in a small array to iterate in source order */
    deflt *stack[128];
    int n = 0;
    for (deflt *d = s->defaults; d && n < 128; d = d->next) stack[n++] = d;
    for (int i = n - 1; i >= 0; i--) {
        if (expr_eval(stack[i]->cond)) { val = stack[i]->val; found = 1; break; }
    }
    (void)found;
    return val;
}

static void resolve(void) {
    /* Outer fixpoint over symbol values (they reference each other via
     * depends/default). Each pass: (1) snapshot, (2) recompute base values
     * from user/default gated by depends, (3) let selects push targets *up*
     * to an inner fixpoint. Because selects only ever raise a value within a
     * pass and we compare against the snapshot, a selected symbol no longer
     * oscillates against its own (0) base. */
    for (int outer = 0; outer < 1000; outer++) {
        for (symbol *s = order_head; s; s = s->order_next) s->prev = s->value;

        /* (2) base values */
        for (symbol *s = order_head; s; s = s->order_next) {
            int allowed = expr_eval(s->dep);
            if (s->is_int)
                s->value = s->user_set ? s->user_value : sym_default(s);
            else
                s->value = allowed ? (s->user_set ? (s->user_value != 0) : sym_default(s)) : 0;
        }
        /* (3) selects raise targets (Linux semantics: a select overrides the
         * target's own depends) */
        for (int inner = 0; inner < 1000; inner++) {
            int raised = 0;
            for (symbol *s = order_head; s; s = s->order_next) {
                if (!s->value) continue;
                for (sel *sl = s->selects; sl; sl = sl->next)
                    if (expr_eval(sl->cond) && !sl->target->value) {
                        sl->target->value = 1;
                        raised = 1;
                    }
            }
            if (!raised) break;
        }

        int changed = 0;
        for (symbol *s = order_head; s; s = s->order_next)
            if (s->value != s->prev) { changed = 1; break; }
        if (!changed) return;
    }
    die("config did not converge (dependency cycle?)");
}

/* ---- .config read/write ---------------------------------------------- */

static void set_user(const char *name, int val) {
    if (!strncmp(name, "CONFIG_", 7)) name += 7; /* .config keys carry the prefix */
    symbol *s = sym_get(name);
    s->user_set = 1;
    s->user_value = val;
}

static void read_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open config '%s'", path);
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *t = trim(line);
        if (!strncmp(t, "# CONFIG_", 9)) {
            char *sp = strchr(t + 2, ' ');            /* "# CONFIG_X is not set" */
            if (sp) { *sp = 0; set_user(t + 2, 0); }
            continue;
        }
        if (!strncmp(t, "CONFIG_", 7)) {
            char *eq = strchr(t, '=');
            if (!eq) continue;
            *eq = 0;
            set_user(t, !strcmp(eq + 1, "y") ? 1 : (int)strtol(eq + 1, NULL, 0));
        }
    }
    fclose(f);
}

static void write_config(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write '%s'", path);
    fputs("# Automatically generated by kconf. Do not edit.\n", f);
    for (symbol *s = order_head; s; s = s->order_next) {
        if (s->is_int)       fprintf(f, "CONFIG_%s=%d\n", s->name, s->value);
        else if (s->is_bool) {
            if (s->value) fprintf(f, "CONFIG_%s=y\n", s->name);
            else          fprintf(f, "# CONFIG_%s is not set\n", s->name);
        }
    }
    fclose(f);
}

static void gen_autoconf(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write '%s'", path);
    fputs("/* Automatically generated by kconf. Do not edit. */\n", f);
    fputs("#ifndef AEGIS_AUTOCONF_H\n#define AEGIS_AUTOCONF_H\n", f);
    for (symbol *s = order_head; s; s = s->order_next) {
        if (s->is_int)              fprintf(f, "#define CONFIG_%s %d\n", s->name, s->value);
        else if (s->is_bool && s->value) fprintf(f, "#define CONFIG_%s 1\n", s->name);
    }
    fputs("#endif\n", f);
    fclose(f);
}

static void gen_autoconf_mk(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write '%s'", path);
    fputs("# Automatically generated by kconf. Do not edit.\n", f);
    for (symbol *s = order_head; s; s = s->order_next) {
        if (s->is_int)              fprintf(f, "CONFIG_%s=%d\n", s->name, s->value);
        else if (s->is_bool && s->value) fprintf(f, "CONFIG_%s=y\n", s->name);
    }
    fclose(f);
}

/* ---- driver ----------------------------------------------------------- */

/* force every prompted bool to a fixed user value (all{no,yes}config) */
static void force_all(int val) {
    for (symbol *s = order_head; s; s = s->order_next)
        if (s->is_bool && s->has_prompt) { s->user_set = 1; s->user_value = val; }
}

static const char *USAGE =
    "usage: kconf <command> [Kconfig]\n"
    "  defconfig <file> [Kconfig]   seed from a defconfig, resolve, write .config\n"
    "  allnoconfig [Kconfig]        smallest: all prompts off\n"
    "  allyesconfig [Kconfig]       all prompts on\n"
    "  oldconfig [Kconfig]          keep .config values, fill new symbols\n"
    "  syncconfig [Kconfig]         .config -> autoconf.h + auto.conf\n"
    "env: KCONFIG (default 'Kconfig'), KCONFIG_CONFIG (default '.config'),\n"
    "     KCONFIG_AUTOHEADER (default 'include/generated/autoconf.h'),\n"
    "     KCONFIG_AUTOCONF   (default 'include/config/auto.conf')\n";

static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && *v) ? v : d;
}

int main(int argc, char **argv) {
    if (argc < 2) { fputs(USAGE, stderr); return 2; }
    const char *cmd = argv[1];
    const char *cfg = env_or("KCONFIG_CONFIG", ".config");
    const char *kconfig = env_or("KCONFIG", "Kconfig");
    const char *autohdr = env_or("KCONFIG_AUTOHEADER", "include/generated/autoconf.h");
    const char *automk = env_or("KCONFIG_AUTOCONF", "include/config/auto.conf");

    if (!strcmp(cmd, "defconfig")) {
        if (argc < 3) die("defconfig needs a file");
        if (argc >= 4) kconfig = argv[3];
        parse_file(kconfig);
        read_config(argv[2]);
        resolve();
        write_config(cfg);
        fprintf(stderr, "kconf: wrote %s from %s\n", cfg, argv[2]);
        return 0;
    }
    if (!strcmp(cmd, "allnoconfig") || !strcmp(cmd, "allyesconfig")) {
        if (argc >= 3) kconfig = argv[2];
        parse_file(kconfig);
        force_all(!strcmp(cmd, "allyesconfig"));
        resolve();
        write_config(cfg);
        fprintf(stderr, "kconf: wrote %s (%s)\n", cfg, cmd);
        return 0;
    }
    if (!strcmp(cmd, "oldconfig")) {
        if (argc >= 3) kconfig = argv[2];
        parse_file(kconfig);
        read_config(cfg);
        resolve();
        write_config(cfg);
        return 0;
    }
    if (!strcmp(cmd, "syncconfig")) {
        if (argc >= 3) kconfig = argv[2];
        parse_file(kconfig);
        read_config(cfg);
        resolve();
        gen_autoconf(autohdr);
        gen_autoconf_mk(automk);
        fprintf(stderr, "kconf: generated %s + %s\n", autohdr, automk);
        return 0;
    }
    fputs(USAGE, stderr);
    return 2;
}
