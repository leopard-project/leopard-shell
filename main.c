/*
 * lpsh — Leopard Shell
 * userspace shell: workdir concept + reads the path variable to resolve commands (replaces hardcoded /apps).
 *
 * Input is read as whole lines (fd 0); the PTY slave's canonical line discipline handles echo/backspace.
 * Child processes inherit 0/1/2 via fork/exec, avoiding new kernel tasks falling back to /dev/stdio.
 * The input method (leopard-ime) is a separate layer after conhost; lpsh only consumes whole-line text.
 */
#include "xtuiapi.h"
#include "libsys.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif

#define LINE_CAP   256
#define MAX_ARGS   32
#define PATH_CAP   512
#define CONFIG_CAP 512
#define USER_CAP   64

#define DEFAULT_PATH "/apps/system:/apps/leopard"

static char g_home[PATH_CAP];
static char g_workdir[PATH_CAP];
static char g_path[CONFIG_CAP];
static char **g_envp;

static int config_get(const char *, const char *, char *, size_t);
static void normalize_path(char *, size_t, const char *);
static inline int64_t spawn(char *path, char *args[])
{
        /* fork/exec keeps fd 0/1/2 on the PTY slave.  xapi_RunArgs creates a
         * fresh task whose stdio falls back to /dev/stdio, which bypasses the
         * PTY and breaks fd-based commands. */
        pid_t pid = fork();

        if (pid < 0)
                return -1;
        if (pid == 0) {
                execve(path, args, g_envp);
                _exit(127);
        }
        return (int64_t)pid;
}
static int64_t run_command(char *args[]);
static void do_cd(char *arg);
static inline int read_line(char *buf, size_t cap)
{
        /* fd stdio: the shell runs on the PTY slave (fd 0), and the slave's
         * canonical line discipline provides echo/backspace.  XAPI_INPUT is
         * no longer the shell's primary input path. */
        ssize_t got;

        if (buf == NULL || cap < 2)
                return 0;
        buf[0] = '\0';

        got = read(STDIN_FILENO, buf, cap - 1);
        if (got == 0)
                return -1; /* EOF */
        if (got < 0)
                return 0;
        while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r'))
                buf[--got] = '\0';
        buf[got] = '\0';
        return (int)got;
}

int main(int argc, char *argv[], char *envp[])
{
        (void)argc;
        (void)argv;
        (void)envp;

        g_envp = envp;

        /* current logged-in user */
        UserInfo ui;
        memset(&ui, 0, sizeof(ui));
        xapi_GetCurrentUser(&ui);
        const char *username = ui.name[0] != '\0' ? ui.name : "user";

        /* home = /users/<username> */
        snprintf(g_home, sizeof(g_home), "/users/%s", username);
        strncpy(g_workdir, g_home, sizeof(g_workdir) - 1);
        g_workdir[sizeof(g_workdir) - 1] = '\0';
        enter_syscall(SYS_CHDIR, (uint64_t)g_home, 0, 0, 0, 0, 0);

        /* path variable (use the default if it cannot be read) */
        if (config_get(username, "path", g_path, sizeof(g_path)) != 0) {
                strncpy(g_path, DEFAULT_PATH, sizeof(g_path) - 1);
                g_path[sizeof(g_path) - 1] = '\0';
        }

        for (;;) {
                char line[LINE_CAP] = {0};
                printf("lpsh$ ");
                if (read_line(line, sizeof(line)) < 0) {
                        printf("\n");
                        return 0;
                }
                if (line[0] == '\0')
                        continue;

                char *args[MAX_ARGS];
                int n = 0;
                for (char *tok = strtok(line, " \t"); tok != NULL && n < MAX_ARGS - 1;
                                tok = strtok(NULL, " \t"))
                        args[n++] = tok;
                args[n] = NULL;
                if (n == 0)
                        continue;

                if (strcmp(args[0], "exit") == 0)
                        return 0;
                if (strcmp(args[0], "clear") == 0) {
                        printf("\033[2J\033[H");
                        continue;
                }
                if (strcmp(args[0], "pwd") == 0) {
                        printf("%s\n", g_workdir);
                        continue;
                }
                if (strcmp(args[0], "cd") == 0) {
                        do_cd(args[1]);
                        continue;
                }

                int64_t pid = run_command(args);
                if (pid < 0) {
                        printf("command not found: %s\n", args[0]);
                } else {
                        int status = 0;
                        enter_syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)&status, 0, 0, 0, 0);
                }
        }
}

static int config_get(const char *username, const char *key, char *out, size_t out_cap)
{
        /* Read a key= value from /users/<username>/config; return 0 on success, -1 on failure */
        char config_path[PATH_CAP];
        snprintf(config_path, sizeof(config_path), "/users/%s/config", username);

        char buf[CONFIG_CAP];
        memset(buf, 0, sizeof(buf));
        int64_t n = (int64_t)xapi_ReadFile((WSTR)config_path, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
                return -1;

        size_t key_len = strlen(key);
        char *line = buf;
        while (*line != '\0') {
                char *eol = strchr(line, '\n');
                if (eol != NULL)
                        *eol = '\0';

                if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
                        if (eol == NULL)
                                break;
                        line = eol + 1;
                        continue;
                }
                
                char *val = line + key_len + 1;
                size_t len = strlen(val);
                while (len > 0 && (val[len - 1] == '\r'
                                   || val[len - 1] == ' '
                                   || val[len - 1] == '\t'))
                        val[--len] = '\0';
                if (len == 0 || len >= out_cap)
                        return -1;
                memcpy(out, val, len);
                out[len] = '\0';
                return 0;
        }
        return -1;
}

static void normalize_path(char *dst, size_t dst_cap, const char *src)
{
        /* Normalize an absolute path in place: fold /./ and /../ */
        char tmp[PATH_CAP];
        strncpy(tmp, src, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        const char *segs[MAX_ARGS];
        int nseg = 0;
        for (char *tok = strtok(tmp, "/"); tok != NULL; tok = strtok(NULL, "/")) {
                if (tok[0] == '\0' || strcmp(tok, ".") == 0)
                        continue;
                if (strcmp(tok, "..") == 0) {
                        if (nseg > 0) nseg--;
                        continue;
                }
                segs[nseg++] = tok;
        }

        if (nseg == 0) {
                snprintf(dst, dst_cap, "/");
                return;
        }
        dst[0] = '\0';
        for (int i = 0; i < nseg; i++) {
                strcat(dst, "/");
                strcat(dst, segs[i]);
        }
}

static int file_exists(const char *path)
{
        char byte = 0;
        return path != NULL && (int64_t)xapi_ReadFile((WSTR)path, &byte, 1, 0) > 0;
}

static int64_t run_command(char *args[])
{
        /* Start a process; return pid on success, negative on failure */
        char cmd[PATH_CAP];
        strncpy(cmd, args[0], sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';

        /* Absolute path: probe first, to avoid the kernel printing "Cannot Open File" for an expected miss */
        if (cmd[0] == '/') {
                if (!file_exists(cmd)) return -1;
                return spawn(cmd, args);
        }

        /* Contains '/': resolve relative to workdir */
        if (strchr(cmd, '/') != NULL) {
                char joined[PATH_CAP];
                snprintf(joined, sizeof(joined), "%s/%s", g_workdir, cmd);
                char norm[PATH_CAP];
                normalize_path(norm, sizeof(norm), joined);
                if (!file_exists(norm)) return -1;
                return spawn(norm, args);
        }

        /* Bare command name: search item by item through the path variable */
        char path_copy[CONFIG_CAP];
        strncpy(path_copy, g_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        for (char *entry = strtok(path_copy, ":"); entry != NULL; entry = strtok(NULL, ":")) {
                char cand[PATH_CAP];
                snprintf(cand, sizeof(cand), "%s/%s", entry, cmd);
                if (!file_exists(cand)) {
                        snprintf(cand, sizeof(cand), "%s/%s.elf", entry, cmd);
                        if (!file_exists(cand)) continue;
                }
                return spawn(cand, args);
        }
        return -1;
}

static void do_cd(char *arg)
{
        if (arg == NULL || arg[0] == '\0') {
                strncpy(g_workdir, g_home, sizeof(g_workdir) - 1);
                g_workdir[sizeof(g_workdir) - 1] = '\0';
                return;
        }

        char joined[PATH_CAP];
        if (arg[0] == '/') {
                snprintf(joined, sizeof(joined), "%s", arg);
        } else {
                snprintf(joined, sizeof(joined), "%s/%s", g_workdir, arg);
        }
        char norm[PATH_CAP];
        normalize_path(norm, sizeof(norm), joined);
        if ((int64_t)enter_syscall(SYS_CHDIR, (uint64_t)norm, 0, 0, 0, 0, 0) != 0) {
                printf("cd: %s: no such directory\n", norm);
                return;
        }
        strncpy(g_workdir, norm, sizeof(g_workdir) - 1);
        g_workdir[sizeof(g_workdir) - 1] = '\0';
}

