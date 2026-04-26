/*
    Copyright (C) 1987-2002 Free Software Foundation, Inc.
    Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
    Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>

    This is based on readline's fileman.c example, which is very useful.
    The original fileman.c carries the following notice:

    This file is part of the GNU Readline Library, a library for
    reading lines of text with interactive input and history editing.

    The GNU Readline Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2, or
    (at your option) any later version.

    The GNU Readline Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    The GNU General Public License is often shipped with GNU software, and
    is generally kept in a file called COPYING or LICENSE.  If you do not
    have a copy of the license, write to the Free Software Foundation,
    59 Temple Place, Suite 330, Boston, MA 02111 USA.
*/

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#ifdef HAVE_LIBREADLINE
#include <readline/readline.h>
#include <readline/history.h>
#elif defined(HAVE_LIBEDIT)
#include <editline/readline.h>
#include <histedit.h>
#endif
#include <getopt.h>
#include <ctype.h>
#include <signal.h>

#ifdef HAVE_LIBBSD
#include <bsd/string.h>
#endif

#include "afp.h"
#include "libafpclient.h"
#include "utils.h"
#include "cmdline_afp.h"
#include "cmdline_testafp.h"

static int running = 1;
static int loop_started = 0;

static pthread_cond_t connected_condition;
static pthread_cond_t loop_started_condition;

extern int com_testafp(char * arg);

static struct termios save_termios;

#ifndef whitespace
#define whitespace(c) (((c) == ' ') || ((c) == '\t'))
#endif

/* A structure which contains information on the commands this program
 *  *    can understand. */

typedef struct {
    char *name;          /* User printable name of the function. */
    int (*func)(char * arg); /* Function to call to do the job. */
    char *doc;           /* Documentation for this function.  */
    int thread;          /* whether to launch as a new thread */
} COMMAND;

void trigger_connected(void)
{
    pthread_cond_signal(&connected_condition);
}

static int tty_reset(int fd)
{
    if (tcsetattr(fd, TCSAFLUSH, &save_termios) < 0) {
        return -1;
    }

    return 0;
}


/* Strip whitespace from the start and end of STRING.  Return a pointer
   into STRING. */
static char *stripwhite(char * string)
{
    char *s, *t;

    for (s = string; whitespace(*s); s++);

    if (*s == 0) {
        return (s);
    }

    t = s + strlen(s) - 1;

    while (t > s && whitespace(*t)) {
        t--;
    }

    *++t = '\0';
    return s;
}

/* **************************************************************** */
/*                                                                  */
/*                  Interface to Readline Completion                */
/*                                                                  */
/* **************************************************************** */

static char *command_generator(const char *, int);

#ifdef HAVE_LIBREADLINE
/* Return non-zero if the character at eindex in string is preceded by an
   odd number of backslashes, meaning it is backslash-escaped and should
   not be treated as a word-break or quote character by readline. */
static int char_is_quoted(char *string, int eindex)
{
    int count = 0;
    int i = eindex - 1;

    while (i >= 0 && string[i] == '\\') {
        count++;
        i--;
    }

    return count & 1;
}
#endif

/* Attempt to complete on the contents of TEXT.  START and END bound the
   region of rl_line_buffer that contains the word to complete.  TEXT is
   the word to complete.  We can use the entire contents of rl_line_buffer
   in case we want to do some simple parsing.  Return the array of matches,
   or NULL if there aren't any. */
static char **filename_completion(const char *text,
                                  int start, __attribute__((unused)) int end)
{
    char **matches = NULL;

    /* If this word is at the start of the line, then it is a command
    to complete.  Otherwise it is the name of a file in the current
    directory. */
    if (start == 0) {
        matches = rl_completion_matches(text, command_generator);
    } else {
        char *line = strdup(rl_line_buffer);
        const char *cmd = strtok(line, " \t");
        int remote = 0;

        /* Check if the command is one that expects remote files */
        if (cmd && (strcmp(cmd, "cat") == 0 ||
                    strcmp(cmd, "cd") == 0 ||
                    strcmp(cmd, "chmod") == 0 ||
                    strcmp(cmd, "cp") == 0 ||
                    strcmp(cmd, "get") == 0 ||
                    strcmp(cmd, "ls") == 0 ||
                    strcmp(cmd, "mkdir") == 0 ||
                    strcmp(cmd, "mv") == 0 ||
                    strcmp(cmd, "rm") == 0 ||
                    strcmp(cmd, "rmdir") == 0 ||
                    strcmp(cmd, "touch") == 0)) {
            remote = 1;
        }

        free(line);

        if (remote) {
            matches = rl_completion_matches(text, afp_remote_file_generator);
        }
    }

    return (matches);
}

/* Tell the GNU Readline library how to complete.  We want to try to complete
   on command names if this is the first word in the line, or on filenames
   if not. */
static void initialize_readline(void)
{
    /* Allow conditional parsing of the ~/.inputrc file. */
    rl_readline_name = "afpfsd";
    /* Tell the completer that we want a crack first. */
    rl_attempted_completion_function = filename_completion;
    /* Tell readline how to detect backslash-escaped characters so it does
       not treat an escaped space as a word-break when finding the start of
       the word to complete (e.g. "cd asdf\ q<TAB>" completes correctly).
       Only available in GNU readline; libedit is handled in the generator. */
#ifdef HAVE_LIBREADLINE
    rl_char_is_quoted_p = char_is_quoted;
#endif
}

/* The user wishes to quit using this program.  Just set DONE non-zero. */
static int com_quit(__attribute__((unused)) char *arg)
{
    cmdline_afp_exit();
    running = 0;
    return 0;
}

/* Explicitly detach volume and terminate server connection, then exit. */
static int com_close(__attribute__((unused)) char *arg)
{
    com_disconnect(NULL);
    running = 0;
    return 0;
}

static int com_help(char *arg);

COMMAND commands[] = {
    { "cat", com_view, "View the contents of FILE", 1 },
    { "cd", com_cd, "Change to directory DIR", 1 },
    { "chmod", com_chmod, "Change mode to MODE on FILE", 1 },
    { "cp", com_copy, "Copy FILE to NEWFILE", 1 },
    { "disconnect", com_close, "Close server connection and shut down afpcmd", 0 },
    { "df", com_statvfs, "Get volume space information", 1 },
    { "exit", com_exit, "Detach from the current volume", 0 },
    { "get", com_get, "Retrieve the file FILE and store them locally", 1 },
    { "help", com_help, "Display this text", 0 },
    { "lcd", com_lcd, "Change local directory to DIR", 1 },
    { "lpwd", com_lpwd, "Print the current local working directory", 0 },
    { "ls", com_dir, "List files in DIR", 1 },
    { "mkdir", com_mkdir, "Make directory DIRECTORY", 1 },
    { "mv", com_rename, "Rename FILE to NEWNAME", 1 },
    { "passwd", com_pass, "Change the password of the current user", 1 },
    { "put", com_put, "Send FILE to the server", 1 },
    { "pwd", com_pwd, "Print the current working directory on the server", 0 },
    { "quit", com_quit, "Shut down afpcmd (leaves server connections intact)", 0 },
    { "rm", com_delete, "Delete FILE", 1 },
    { "rmdir", com_rmdir, "Remove directory DIRECTORY", 1 },
    { "status", com_status, "Get some server status", 1 },
    { "touch", com_touch, "Touch FILE", 1 },
    { "?", com_help, "Same as `help'", 0 },
#ifdef DEBUG
    { "test", test_urls, "Run client tests", 1},
#endif
    { (char *)NULL, NULL, (char *)NULL, 0 }
};

/* Generator function for command completion.  STATE lets us know whether
   to start from scratch; without any state (i.e. STATE == 0), then we
   start at the top of the list. */
static char *command_generator(const char *text, int state)
{
    static int list_index, len;
    char *name;

    /* If this is a new word to complete, initialize now.  This includes
    saving the length of TEXT for efficiency, and initializing the index
    variable to 0. */
    if (!state) {
        list_index = 0;

        if (!text) {
            return NULL;  /* No text to match */
        }

        len = strnlen(text, ARG_LEN);
    }

    /* Return the next name which partially matches from the command list. */
    while ((name = commands[list_index].name)) {
        list_index++;

        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }

    /* If no names matched, then return NULL. */
    return ((char *)NULL);
}

/* Print out help for ARG, or for all of the commands if ARG is
   not present. */
static int com_help(char *arg)
{
    register int i;
    int printed = 0;

    for (i = 0; commands[i].name; i++) {
        if (!*arg || (strcmp(arg, commands[i].name) == 0)) {
            printf("  %-12s  %s\n", commands[i].name, commands[i].doc);
            printed++;
        }
    }

    if (!printed) {
        printf("No commands match `%s'.  Possibilties are:\n", arg);

        for (i = 0; commands[i].name; i++) {
            /* Print in six columns. */
            if (printed == 6) {
                printed = 0;
                printf("\n");
            }

            printf("%s\t", commands[i].name);
            printed++;
        }

        if (printed) {
            printf("\n");
        }
    }

    return (0);
}

/* Look up NAME as the name of a command, and return a pointer to that
   command.  Return a NULL pointer if NAME isn't a command name. */
static COMMAND *find_command(char *name)
{
    int i;

    for (i = 0; commands[i].name; i++)
        if (strcmp(name, commands[i].name) == 0) {
            return (&commands[i]);
        }

    return ((COMMAND *)NULL);
}

/* Execute a command line. */
static int execute_line(char * line)
{
    int i;
    COMMAND *command;
    char *word;
    /* Isolate the command word. */
    i = 0;

    while (line[i] && whitespace(line[i])) {
        i++;
    }

    word = line + i;

    while (line[i] && !whitespace(line[i])) {
        i++;
    }

    if (line[i]) {
        line[i++] = '\0';
    }

    command = find_command(word);

    if (!command) {
        fprintf(stderr, "%s: No such command.\n", word);
        return (-1);
    }

    /* Get argument to command, if any. */
    while (whitespace(line[i])) {
        i++;
    }

    word = line + i;
    /* Call the function. */
    command->func(word);
    return 0;
}

void *cmdline_ui(__attribute__((unused)) void * other)
{
    char *line;
    char *s, s2[ARG_LEN];

    while (running)  {
        line = readline("afpcmd: ");

        if (!line) {
            return 0;
        }

        /* Remove leading and trailing whitespace from the line.
        Then, if there is anything left, add it to the history list
        and execute it. */
        s = stripwhite(line);
        strlcpy(s2, s, ARG_LEN);

        if (*s) {
            add_history(s);
            execute_line(s2);
        }

        free(line);
    }

    return 0;
}

static void ending(void)
{
    if (full_url == 0) {
        printf("Forced exit\n");
    }

    cmdline_afp_exit();
    tty_reset(STDIN_FILENO);
    exit(1);
}

void cmdline_forced_ending_hook(void)
{
    ending();
}

static volatile int interrupt_count = 0;

void earlyexit_handler(__attribute__((unused)) int signum)
{
    sigset_t mask;

    if (interrupt_count > 0) {
        const char msg[] = "\nImmediate exit requested.\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        tty_reset(STDIN_FILENO);
        _exit(1);
    }

    interrupt_count++;
    /* Unblock SIGINT so we can catch a second Ctrl+C */
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    ending();
}

void cmdline_loop_started(void)
{
    loop_started = 1;
    pthread_cond_signal(&loop_started_condition);
}

static void usage(void)
{
    printf(
        "afpfs-ng %s - Apple Filing Protocol CLI client application\n"
        "afpcmd [-h] [-r] [-V] [-v loglevel] <afp url>\n"
        "Options:\n"
        "\t-h:          show this help message\n"
        "\t-r:          set the recursive flag\n"
        "\t-V:          verbose mode (show detailed transfer messages)\n"
        "\t-v loglevel: set log verbosity (debug, info, notice, warning, error)\n"
        "\turl:         an AFP url, in the form of:\n"
        "\t\t         afp://username;AUTH=uamname:password@server:548/volume/path\n"
        "\t             uamname can be a full UAM name or shorthand:\n"
        "\t             guest, clrtxt, randnum, randnum2, dhx, dhx2\n\n"
        "Batch transfer mode:\n"
        "\tafpcmd [-r] [-V] <afp url> <local path>   (Download from server)\n"
        "\tafpcmd [-r] [-V] <local path> <afp url>   (Upload to server)\n\n"
        "See the afpcmd(1) man page for more information.\n", AFPFS_VERSION
    );
}


int main(int argc, char *argv[])
{
    int option_index = 0;
    int c;
    int recursive = 0;
    int verbose = 0;
    int show_usage = 0;
    int log_level = LOG_NOTICE;
    int dsi_timeout = 0;
    struct option long_options[] = {
        {"help", 0, 0, 'h'},
        {"recursive", 0, 0, 'r'},
        {"verbose", 0, 0, 'V'},
        {"loglevel", 1, 0, 'v'},
        {"timeout", 1, 0, 't'},
        {NULL, 0, NULL, 0},
    };
    char *url = NULL;
    char *local_path = NULL;
    int batch_mode = 0;
    int direction = 0; /* 0 = GET (remote->local), 1 = PUT (local->remote) */

    while (1) {
        c = getopt_long(argc, argv, "hrVv:t:",
                        long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            show_usage = 1;
            break;

        case 'r':
            recursive = 1;
            break;

        case 'V':
            verbose = 1;
            break;

        case 't':
            dsi_timeout = strtol(optarg, NULL, 10);
            break;

        case 'v': {
            int parsed_loglevel;

            if (string_to_log_level(optarg, &parsed_loglevel) != 0) {
                printf("Unknown log level %s\n", optarg);
                usage();
                return -1;
            }

            log_level = parsed_loglevel;
            break;
        }

        default:
            show_usage = 1;
            break;
        }
    }

    if (show_usage) {
        usage();
        exit(0);
    }

    /* Setup client before parsing URLs to avoid segfault in logging */
    cmdline_afp_setup_client();
    cmdline_set_log_level(log_level);
    cmdline_set_verbose(verbose);
    cmdline_set_dsi_timeout(dsi_timeout);

    /* Check arguments for batch mode */
    if (argc - optind == 2) {
        struct afp_url tmp_url;
        char *arg1 = argv[optind];
        char *arg2 = argv[optind + 1];

        /* Check if first arg is URL */
        if (afp_parse_url(&tmp_url, arg1) == 0) {
            url = arg1;
            local_path = arg2;
            batch_mode = 1;
            direction = 0; /* GET */
        }
        /* Check if second arg is URL */
        else if (afp_parse_url(&tmp_url, arg2) == 0) {
            local_path = arg1;
            url = arg2;
            batch_mode = 1;
            direction = 1; /* PUT */
        } else {
            printf("Neither argument appears to be a valid AFP URL.\n");
            usage();
            exit(1);
        }
    } else if (optind < argc) {
        url = argv[optind];
    }

    if (!url) {
        usage();
        exit(1);
    }

    tcgetattr(STDIN_FILENO, &save_termios);
    initialize_readline();

    if (cmdline_afp_setup(recursive, batch_mode, url) != 0) {
        cmdline_afp_exit();
        tty_reset(STDIN_FILENO);
        exit(1);
    }

    signal(SIGINT, earlyexit_handler);

    if (batch_mode) {
        if (cmdline_batch_transfer(local_path, direction, recursive) < 0) {
            cmdline_afp_exit();
            tty_reset(STDIN_FILENO);
            exit(1);
        }

        cmdline_afp_exit();
    } else {
        cmdline_ui(NULL);
    }

    tty_reset(STDIN_FILENO);
    exit(0);
}
