#include <shell/gwbasic.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/fb.h>
#include <hal/cpu.h>
#include <drivers/keyboard.h>
#include <hal/serial.h>
#include <sched/sched.h>
#include <mm/heap.h>
#include <fs/vfs.h>

#define MAX_LINE_LEN   160
#define MAX_VARS       128
#define MAX_FOR_STACK  16
#define MAX_GOSUB_STACK 32

typedef enum {
    VAL_NUM,
    VAL_STR
} val_type_t;

typedef struct {
    val_type_t type;
    int64_t num;
    char str[128];
} basic_val_t;

typedef struct {
    char name[32];
    basic_val_t val;
} basic_var_t;

typedef struct basic_line {
    int num;
    char text[MAX_LINE_LEN];
    struct basic_line *next;
} basic_line_t;

typedef struct {
    char var_name[32];
    int64_t target;
    int64_t step;
    basic_line_t *line;
    char resume_text[MAX_LINE_LEN];
} for_frame_t;

typedef struct {
    basic_line_t *line;
    char resume_text[MAX_LINE_LEN];
} gosub_frame_t;

// Interpreter State
static basic_line_t *prog_head = NULL;
static basic_var_t vars[MAX_VARS];
static int var_count = 0;

static for_frame_t for_stack[MAX_FOR_STACK];
static int for_sp = 0;

static gosub_frame_t gosub_stack[MAX_GOSUB_STACK];
static int gosub_sp = 0;

static bool prog_running = false;
static basic_line_t *current_exec_line = NULL;
static uint32_t rnd_seed = 123456789;

// Helper: Custom integer math functions
static int64_t basic_sqrt(int64_t x) {
    if (x <= 0) return 0;
    int64_t res = x;
    for (int i = 0; i < 20; i++) {
        if (res == 0) break;
        res = (res + x / res) / 2;
    }
    return res;
}

static int64_t basic_rnd(int64_t max) {
    rnd_seed = rnd_seed * 1103515245 + 12345;
    uint32_t val = (rnd_seed / 65536) % 32768;
    if (max <= 0) return (int64_t)val;
    return (int64_t)(val % max);
}

static inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static int strcasecmp_custom(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = to_upper(*s1++);
        char c2 = to_upper(*s2++);
        if (c1 != c2) return (int)(unsigned char)c1 - (int)(unsigned char)c2;
    }
    return (int)(unsigned char)to_upper(*s1) - (int)(unsigned char)to_upper(*s2);
}

static int strncasecmp_custom(const char *s1, const char *s2, size_t n) {
    while (n > 0) {
        if (*s1 == '\0' || *s2 == '\0') {
            return (int)(unsigned char)to_upper(*s1) - (int)(unsigned char)to_upper(*s2);
        }
        char c1 = to_upper(*s1);
        char c2 = to_upper(*s2);
        if (c1 != c2) return (int)(unsigned char)c1 - (int)(unsigned char)c2;
        s1++;
        s2++;
        n--;
    }
    return 0;
}

// Variables Management
static basic_var_t *get_var(const char *name, bool create) {
    char uname[32];
    int i = 0;
    while (name[i] && i < 31) {
        uname[i] = to_upper(name[i]);
        i++;
    }
    uname[i] = '\0';

    // 1. Direct match
    for (int v = 0; v < var_count; v++) {
        if (strcmp(vars[v].name, uname) == 0) {
            return &vars[v];
        }
    }

    // 2. If name starts with '$' (e.g. $I or $COUNT), check if base variable (I or COUNT) exists
    if (uname[0] == '$' && uname[1] != '\0') {
        for (int v = 0; v < var_count; v++) {
            if (strcmp(vars[v].name, &uname[1]) == 0) {
                return &vars[v];
            }
        }
    }

    // 3. If base variable was requested (e.g. I), check if $I exists
    for (int v = 0; v < var_count; v++) {
        if (vars[v].name[0] == '$' && strcmp(&vars[v].name[1], uname) == 0) {
            return &vars[v];
        }
    }

    if (!create || var_count >= MAX_VARS) return NULL;

    basic_var_t *nv = &vars[var_count++];
    if (uname[0] == '$' && uname[1] != '\0') {
        strcpy(nv->name, &uname[1]);
    } else {
        strcpy(nv->name, uname);
    }

    if (nv->name[strlen(nv->name) - 1] == '$') {
        nv->val.type = VAL_STR;
        nv->val.str[0] = '\0';
    } else {
        nv->val.type = VAL_NUM;
        nv->val.num = 0;
    }
    return nv;
}

static void clear_vars(void) {
    var_count = 0;
    for_sp = 0;
    gosub_sp = 0;
}

// Program Lines Management
static void clear_program(void) {
    basic_line_t *curr = prog_head;
    while (curr) {
        basic_line_t *next = curr->next;
        kfree(curr);
        curr = next;
    }
    prog_head = NULL;
    clear_vars();
}

static void insert_line(int num, const char *text) {
    while (is_space(*text)) text++;

    if (*text == '\0') {
        // Delete line
        basic_line_t *curr = prog_head;
        basic_line_t *prev = NULL;
        while (curr) {
            if (curr->num == num) {
                if (prev) prev->next = curr->next;
                else prog_head = curr->next;
                kfree(curr);
                return;
            }
            prev = curr;
            curr = curr->next;
        }
        return;
    }

    // Replace existing line
    basic_line_t *curr = prog_head;
    while (curr) {
        if (curr->num == num) {
            strncpy(curr->text, text, sizeof(curr->text) - 1);
            curr->text[sizeof(curr->text) - 1] = '\0';
            return;
        }
        curr = curr->next;
    }

    // Insert sorted
    basic_line_t *nl = kmalloc(sizeof(basic_line_t));
    if (!nl) return;
    nl->num = num;
    strncpy(nl->text, text, sizeof(nl->text) - 1);
    nl->text[sizeof(nl->text) - 1] = '\0';
    nl->next = NULL;

    if (!prog_head || num < prog_head->num) {
        nl->next = prog_head;
        prog_head = nl;
    } else {
        curr = prog_head;
        while (curr->next && curr->next->num < num) {
            curr = curr->next;
        }
        nl->next = curr->next;
        curr->next = nl;
    }
}

// Expression Parser Forward Declarations
static basic_val_t parse_expr(const char **p);

static const char *skip_spaces(const char *p) {
    while (is_space(*p)) p++;
    return p;
}

static basic_val_t parse_primary(const char **p) {
    *p = skip_spaces(*p);
    basic_val_t v;
    v.type = VAL_NUM;
    v.num = 0;
    v.str[0] = '\0';

    if (**p == '\0') return v;

    if (**p == '"') {
        // String literal
        (*p)++;
        int idx = 0;
        while (**p && **p != '"' && idx < 127) {
            v.str[idx++] = *(*p)++;
        }
        v.str[idx] = '\0';
        if (**p == '"') (*p)++;
        v.type = VAL_STR;
        return v;
    }

    if (is_digit(**p)) {
        // Integer number literal
        int64_t val = 0;
        while (is_digit(**p)) {
            val = val * 10 + (**p - '0');
            (*p)++;
        }
        v.type = VAL_NUM;
        v.num = val;
        return v;
    }

    if (**p == '(') {
        (*p)++;
        v = parse_expr(p);
        *p = skip_spaces(*p);
        if (**p == ')') (*p)++;
        return v;
    }

    if (is_alpha(**p) || **p == '$') {
        // Variable or Function
        char id[32];
        int idx = 0;
        while ((is_digit(**p) || is_alpha(**p) || **p == '$' || **p == '%') && idx < 31) {
            id[idx++] = *(*p)++;
        }
        id[idx] = '\0';

        *p = skip_spaces(*p);
        if (**p == '(') {
            // Function call
            (*p)++;
            basic_val_t arg = parse_expr(p);
            *p = skip_spaces(*p);
            if (**p == ')') (*p)++;

            if (strcasecmp_custom(id, "SQR") == 0) {
                v.type = VAL_NUM;
                v.num = basic_sqrt(arg.num);
                return v;
            } else if (strcasecmp_custom(id, "ABS") == 0) {
                v.type = VAL_NUM;
                v.num = arg.num < 0 ? -arg.num : arg.num;
                return v;
            } else if (strcasecmp_custom(id, "INT") == 0) {
                v.type = VAL_NUM;
                v.num = arg.num;
                return v;
            } else if (strcasecmp_custom(id, "SGN") == 0) {
                v.type = VAL_NUM;
                v.num = arg.num > 0 ? 1 : (arg.num < 0 ? -1 : 0);
                return v;
            } else if (strcasecmp_custom(id, "RND") == 0) {
                v.type = VAL_NUM;
                v.num = basic_rnd(arg.num > 0 ? arg.num : 32767);
                return v;
            } else if (strcasecmp_custom(id, "LEN") == 0) {
                v.type = VAL_NUM;
                v.num = (int64_t)strlen(arg.str);
                return v;
            } else if (strcasecmp_custom(id, "CHR$") == 0) {
                v.type = VAL_STR;
                v.str[0] = (char)arg.num;
                v.str[1] = '\0';
                return v;
            } else if (strcasecmp_custom(id, "ASC") == 0) {
                v.type = VAL_NUM;
                v.num = (int64_t)(unsigned char)arg.str[0];
                return v;
            }
        }

        // Variable lookup
        basic_var_t *var = get_var(id, true);
        if (var) return var->val;
    }

    return v;
}

static basic_val_t parse_unary(const char **p) {
    *p = skip_spaces(*p);
    if (**p == '-') {
        (*p)++;
        basic_val_t v = parse_unary(p);
        if (v.type == VAL_NUM) v.num = -v.num;
        return v;
    } else if (**p == '+') {
        (*p)++;
        return parse_unary(p);
    }
    return parse_primary(p);
}

static basic_val_t parse_mul_div(const char **p) {
    basic_val_t left = parse_unary(p);
    while (1) {
        *p = skip_spaces(*p);
        char op = **p;
        if (op == '*' || op == '/') {
            (*p)++;
            basic_val_t right = parse_unary(p);
            if (op == '*') {
                left.num *= right.num;
            } else {
                if (right.num != 0) left.num /= right.num;
                else left.num = 0;
            }
        } else if (strncasecmp_custom(*p, "MOD", 3) == 0 && is_space((*p)[3])) {
            *p += 3;
            basic_val_t right = parse_unary(p);
            int64_t l = left.num;
            int64_t r = right.num;
            if (r != 0) left.num = l % r;
            else left.num = 0;
        } else {
            break;
        }
    }
    return left;
}

static basic_val_t parse_add_sub(const char **p) {
    basic_val_t left = parse_mul_div(p);
    while (1) {
        *p = skip_spaces(*p);
        char op = **p;
        if (op == '+' || op == '-') {
            (*p)++;
            basic_val_t right = parse_mul_div(p);
            if (op == '+') {
                if (left.type == VAL_STR || right.type == VAL_STR) {
                    // String concatenation
                    left.type = VAL_STR;
                    size_t len = strlen(left.str);
                    strncpy(left.str + len, right.str, sizeof(left.str) - len - 1);
                    left.str[sizeof(left.str) - 1] = '\0';
                } else {
                    left.num += right.num;
                }
            } else {
                left.num -= right.num;
            }
        } else {
            break;
        }
    }
    return left;
}

static basic_val_t parse_relational(const char **p) {
    basic_val_t left = parse_add_sub(p);
    *p = skip_spaces(*p);

    char op[3] = {0, 0, 0};
    if (**p == '=' || **p == '<' || **p == '>') {
        op[0] = *(*p)++;
        if (**p == '=' || **p == '>') {
            op[1] = *(*p)++;
        }
    }

    if (op[0]) {
        basic_val_t right = parse_add_sub(p);
        bool res = false;
        if (strcmp(op, "=") == 0) {
            if (left.type == VAL_STR) res = (strcmp(left.str, right.str) == 0);
            else res = (left.num == right.num);
        } else if (strcmp(op, "<>") == 0) {
            if (left.type == VAL_STR) res = (strcmp(left.str, right.str) != 0);
            else res = (left.num != right.num);
        } else if (strcmp(op, "<") == 0) {
            res = (left.num < right.num);
        } else if (strcmp(op, ">") == 0) {
            res = (left.num > right.num);
        } else if (strcmp(op, "<=") == 0) {
            res = (left.num <= right.num);
        } else if (strcmp(op, ">=") == 0) {
            res = (left.num >= right.num);
        }
        left.type = VAL_NUM;
        left.num = res ? -1 : 0; // BASIC true is -1
    }
    return left;
}

static basic_val_t parse_expr(const char **p) {
    return parse_relational(p);
}

// Basic Input Helper
static char basic_getchar(void) {
    while (1) {
        if (keyboard_has_char()) {
            return keyboard_getchar();
        }
        if (serial_received()) {
            return serial_getchar();
        }
        sched_yield();
        __asm__ volatile ("pause");
    }
}

static void basic_readline(char *buf, size_t max) {
    size_t pos = 0;
    while (1) {
        fb_draw_cursor(true);
        char c = basic_getchar();
        fb_draw_cursor(false);

        if (c == '\r' || c == '\n') {
            printk("\n");
            buf[pos] = '\0';
            return;
        } else if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                printk("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (pos < max - 1) {
                buf[pos++] = c;
                printk("%c", c);
            }
        }
    }
}

// Statement Execution
static bool execute_statement(const char **p) {
    *p = skip_spaces(*p);
    if (**p == '\0') return true;

    // PRINT statement (or '?')
    if (**p == '?' || (strncasecmp_custom(*p, "PRINT", 5) == 0 && (is_space((*p)[5]) || (*p)[5] == '"' || (*p)[5] == '\0' || (*p)[5] == ':'))) {
        if (**p == '?') (*p)++;
        else *p += 5;

        bool trailing_semi = false;
        while (**p && **p != ':') {
            *p = skip_spaces(*p);
            if (**p == '\0' || **p == ':') break;

            if (**p == ';') {
                trailing_semi = true;
                (*p)++;
                continue;
            } else if (**p == ',') {
                printk("    "); // 4-space tab
                trailing_semi = true;
                (*p)++;
                continue;
            }

            trailing_semi = false;
            const char *before = *p;
            basic_val_t val = parse_expr(p);
            if (*p == before) {
                (*p)++;
                break;
            }
            if (val.type == VAL_STR) {
                printk("%s", val.str);
            } else {
                if (val.num >= 0) printk(" %lld ", val.num);
                else printk("%lld ", val.num);
            }
            *p = skip_spaces(*p);
            if (**p == ';' || **p == ',') {
                if (**p == ';') trailing_semi = true;
                else { printk("    "); trailing_semi = true; }
                (*p)++;
            }
        }
        if (!trailing_semi) {
            printk("\n");
        }
        return true;
    }

    // INPUT statement
    if (strncasecmp_custom(*p, "INPUT", 5) == 0 && is_space((*p)[5])) {
        *p += 5;
        *p = skip_spaces(*p);
        if (**p == '"') {
            (*p)++;
            while (**p && **p != '"') {
                printk("%c", *(*p)++);
            }
            if (**p == '"') (*p)++;
            *p = skip_spaces(*p);
            if (**p == ';' || **p == ',') (*p)++;
        } else {
            printk("? ");
        }

        *p = skip_spaces(*p);
        char var_name[32];
        int vi = 0;
        while ((is_alpha(**p) || is_digit(**p) || **p == '$') && vi < 31) {
            var_name[vi++] = *(*p)++;
        }
        var_name[vi] = '\0';

        char input_buf[128];
        basic_readline(input_buf, sizeof(input_buf));

        basic_var_t *var = get_var(var_name, true);
        if (var) {
            if (var->val.type == VAL_STR) {
                strncpy(var->val.str, input_buf, sizeof(var->val.str) - 1);
            } else {
                const char *ip = input_buf;
                basic_val_t iv = parse_expr(&ip);
                var->val.num = iv.num;
            }
        }
        return true;
    }

    // GOTO statement
    if (strncasecmp_custom(*p, "GOTO", 4) == 0 && is_space((*p)[4])) {
        *p += 4;
        *p = skip_spaces(*p);
        int target = 0;
        while (is_digit(**p)) {
            target = target * 10 + (*(*p)++ - '0');
        }
        basic_line_t *l = prog_head;
        while (l && l->num != target) l = l->next;
        if (l) {
            current_exec_line = l;
            *p = l->text;
        } else {
            printk("Undefined line number %d\n", target);
            prog_running = false;
        }
        return true;
    }

    // GOSUB statement
    if (strncasecmp_custom(*p, "GOSUB", 5) == 0 && is_space((*p)[5])) {
        *p += 5;
        *p = skip_spaces(*p);
        int target = 0;
        while (is_digit(**p)) {
            target = target * 10 + (*(*p)++ - '0');
        }
        if (gosub_sp < MAX_GOSUB_STACK) {
            gosub_stack[gosub_sp].line = current_exec_line;
            strncpy(gosub_stack[gosub_sp].resume_text, *p, MAX_LINE_LEN - 1);
            gosub_sp++;
        }
        basic_line_t *l = prog_head;
        while (l && l->num != target) l = l->next;
        if (l) {
            current_exec_line = l;
            *p = l->text;
        } else {
            printk("Undefined line number %d\n", target);
            prog_running = false;
        }
        return true;
    }

    // RETURN statement
    if (strncasecmp_custom(*p, "RETURN", 6) == 0) {
        *p += 6;
        if (gosub_sp > 0) {
            gosub_sp--;
            current_exec_line = gosub_stack[gosub_sp].line;
            static char ret_buf[MAX_LINE_LEN];
            strncpy(ret_buf, gosub_stack[gosub_sp].resume_text, MAX_LINE_LEN - 1);
            *p = ret_buf;
        } else {
            printk("RETURN without GOSUB\n");
            prog_running = false;
        }
        return true;
    }

    // IF statement
    if (strncasecmp_custom(*p, "IF", 2) == 0 && is_space((*p)[2])) {
        *p += 2;
        basic_val_t cond = parse_expr(p);
        *p = skip_spaces(*p);
        if (strncasecmp_custom(*p, "THEN", 4) == 0) {
            *p += 4;
            *p = skip_spaces(*p);
            if (cond.num != 0) {
                if (is_digit(**p)) {
                    int target = 0;
                    while (is_digit(**p)) target = target * 10 + (*(*p)++ - '0');
                    basic_line_t *l = prog_head;
                    while (l && l->num != target) l = l->next;
                    if (l) {
                        current_exec_line = l;
                        *p = l->text;
                    }
                    return true;
                }
                return execute_statement(p);
            } else {
                // Skip to ELSE if present
                while (**p && strncasecmp_custom(*p, "ELSE", 4) != 0) (*p)++;
                if (strncasecmp_custom(*p, "ELSE", 4) == 0) {
                    *p += 4;
                    return execute_statement(p);
                }
                *p = ""; // Done with line
            }
        }
        return true;
    }

    // FOR statement
    if (strncasecmp_custom(*p, "FOR", 3) == 0 && is_space((*p)[3])) {
        *p += 3;
        *p = skip_spaces(*p);
        char var_name[32];
        int vi = 0;
        while ((is_alpha(**p) || is_digit(**p) || **p == '$') && vi < 31) var_name[vi++] = *(*p)++;
        var_name[vi] = '\0';
        *p = skip_spaces(*p);
        if (**p == '=') (*p)++;
        basic_val_t start_val = parse_expr(p);
        *p = skip_spaces(*p);
        if (strncasecmp_custom(*p, "TO", 2) == 0) *p += 2;
        basic_val_t end_val = parse_expr(p);

        int64_t step_val = 1;
        *p = skip_spaces(*p);
        if (strncasecmp_custom(*p, "STEP", 4) == 0) {
            *p += 4;
            basic_val_t sv = parse_expr(p);
            step_val = sv.num;
        }

        basic_var_t *var = get_var(var_name, true);
        if (var) var->val.num = start_val.num;

        if (for_sp < MAX_FOR_STACK) {
            strcpy(for_stack[for_sp].var_name, var_name);
            for_stack[for_sp].target = end_val.num;
            for_stack[for_sp].step = step_val;
            for_stack[for_sp].line = current_exec_line;
            strncpy(for_stack[for_sp].resume_text, *p, MAX_LINE_LEN - 1);
            for_sp++;
        }
        return true;
    }

    // NEXT statement
    if (strncasecmp_custom(*p, "NEXT", 4) == 0) {
        *p += 4;
        *p = skip_spaces(*p);
        char var_name[32] = "";
        int vi = 0;
        while ((is_alpha(**p) || is_digit(**p) || **p == '$') && vi < 31) var_name[vi++] = *(*p)++;
        var_name[vi] = '\0';

        if (for_sp > 0) {
            int frame_idx = for_sp - 1;
            if (var_name[0] != '\0') {
                while (frame_idx >= 0 && strcasecmp_custom(for_stack[frame_idx].var_name, var_name) != 0) {
                    frame_idx--;
                }
            }
            if (frame_idx >= 0) {
                for_frame_t *frame = &for_stack[frame_idx];
                basic_var_t *var = get_var(frame->var_name, false);
                if (var) {
                    var->val.num += frame->step;
                    bool continue_loop = (frame->step > 0) ? (var->val.num <= frame->target) : (var->val.num >= frame->target);
                    if (continue_loop) {
                        current_exec_line = frame->line;
                        static char loop_buf[MAX_LINE_LEN];
                        strncpy(loop_buf, frame->resume_text, MAX_LINE_LEN - 1);
                        *p = loop_buf;
                        return true;
                    } else {
                        for_sp = frame_idx;
                    }
                }
            }
        }
        return true;
    }

    // CLS statement
    if (strncasecmp_custom(*p, "CLS", 3) == 0 && ((*p)[3] == '\0' || is_space((*p)[3]) || (*p)[3] == ':')) {
        *p += 3;
        fb_clear(0x00000000);
        return true;
    }

    // END / STOP statement
    if (strncasecmp_custom(*p, "END", 3) == 0 || strncasecmp_custom(*p, "STOP", 4) == 0) {
        prog_running = false;
        *p = "";
        return true;
    }

    // REM or ' (Comment)
    if (strncasecmp_custom(*p, "REM", 3) == 0 || **p == '\'') {
        *p = "";
        return true;
    }

    // Optional LET or Assignment (e.g. A = 5 or LET A = 5)
    if (strncasecmp_custom(*p, "LET", 3) == 0 && is_space((*p)[3])) {
        *p += 3;
        *p = skip_spaces(*p);
    }

    if (is_alpha(**p) || **p == '$') {
        char var_name[32];
        int vi = 0;
        while ((is_alpha(**p) || is_digit(**p) || **p == '$' || **p == '%') && vi < 31) {
            var_name[vi++] = *(*p)++;
        }
        var_name[vi] = '\0';
        *p = skip_spaces(*p);
        if (**p == '=') {
            (*p)++;
            basic_val_t res = parse_expr(p);
            basic_var_t *var = get_var(var_name, true);
            if (var) {
                var->val = res;
            }
            return true;
        }
    }

    return true;
}

static void run_basic_program(void) {
    prog_running = true;
    current_exec_line = prog_head;
    for_sp = 0;
    gosub_sp = 0;

    while (prog_running && current_exec_line) {
        const char *p = current_exec_line->text;
        while (*p && prog_running) {
            execute_statement(&p);
            p = skip_spaces(p);
            if (*p == ':') p++;
        }
        if (prog_running) {
            current_exec_line = current_exec_line->next;
        }
    }
    prog_running = false;
    printk("Ok\n");
}

static void list_program(int from, int to) {
    basic_line_t *curr = prog_head;
    while (curr) {
        if (curr->num >= from && (to == 0 || curr->num <= to)) {
            printk("%d %s\n", curr->num, curr->text);
        }
        curr = curr->next;
    }
    printk("Ok\n");
}

static void load_basic_file(const char *filename) {
    vfs_node_t *file = vfs_open(filename, 0);
    if (!file) {
        printk("File not found: %s\n", filename);
        return;
    }

    clear_program();
    char buf[2048];
    int bytes = vfs_read(file, 0, sizeof(buf) - 1, buf);
    vfs_close(file);
    if (bytes <= 0) return;
    buf[bytes] = '\0';

    char *line = buf;
    while (*line) {
        char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        char save = *eol;
        *eol = '\0';

        const char *p = skip_spaces(line);
        if (is_digit(*p)) {
            int line_num = 0;
            while (is_digit(*p)) line_num = line_num * 10 + (*p++ - '0');
            insert_line(line_num, p);
        }

        if (save == '\0') break;
        line = eol + 1;
        while (*line == '\n' || *line == '\r') line++;
    }
    printk("Loaded '%s'\nOk\n", filename);
}

void cmd_gwbasic(int argc, char **argv) {
    printk("\n=======================================================\n");
    printk("   GW-BASIC 1.00 - Microsoft (R) 1983 Release         \n");
    printk("   (C) Copyright Microsoft Corp. 1983                 \n");
    printk("   60300 Bytes free                                   \n");
    printk("=======================================================\n\n");
    printk("Ok\n");

    if (argc > 1) {
        load_basic_file(argv[1]);
        run_basic_program();
    }

    char line_buf[MAX_LINE_LEN];

    while (1) {
        basic_readline(line_buf, sizeof(line_buf));
        const char *p = skip_spaces(line_buf);
        if (*p == '\0') continue;

        // Line number detected
        if (is_digit(*p)) {
            int line_num = 0;
            while (is_digit(*p)) {
                line_num = line_num * 10 + (*p++ - '0');
            }
            insert_line(line_num, p);
            continue;
        }

        // Commands
        if (strcasecmp_custom(p, "RUN") == 0) {
            run_basic_program();
        } else if (strncasecmp_custom(p, "LIST", 4) == 0) {
            p += 4;
            p = skip_spaces(p);
            int from = 0;
            while (is_digit(*p)) from = from * 10 + (*p++ - '0');
            list_program(from, 0);
        } else if (strncasecmp_custom(p, "LOAD", 4) == 0) {
            p += 4;
            p = skip_spaces(p);
            if (*p == '"') p++;
            char fname[64];
            int fi = 0;
            while (*p && *p != '"' && fi < 63) fname[fi++] = *p++;
            fname[fi] = '\0';
            load_basic_file(fname);
        } else if (strcasecmp_custom(p, "NEW") == 0) {
            clear_program();
            printk("Ok\n");
        } else if (strcasecmp_custom(p, "CLEAR") == 0) {
            clear_vars();
            printk("Ok\n");
        } else if (strcasecmp_custom(p, "SYSTEM") == 0 || strcasecmp_custom(p, "EXIT") == 0) {
            break;
        } else if (strcasecmp_custom(p, "HELP") == 0) {
            printk("GW-BASIC Commands:\n");
            printk("  RUN              - Execute program lines\n");
            printk("  LIST [line]      - Display program lines\n");
            printk("  LOAD \"file.bas\"  - Load program from ext4\n");
            printk("  NEW              - Clear program buffer\n");
            printk("  CLEAR            - Clear variables\n");
            printk("  SYSTEM           - Return to Antigravity OS shell\n");
            printk("  Statements: PRINT, INPUT, IF/THEN/ELSE, FOR/TO/STEP/NEXT, GOTO, GOSUB, RETURN, CLS, REM\n");
            printk("Ok\n");
        } else {
            // Immediate statement execution
            execute_statement(&p);
            printk("Ok\n");
        }
    }
}
