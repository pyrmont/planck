#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "linenoise.h"

#include "engine.h"
#include "globals.h"
#include "keymap.h"
#include "sockets.h"
#include "str.h"
#include "theme.h"
#include "timers.h"

struct repl {
    char *current_ns;
    char *current_prompt;
    char *history_path;
    char *input;
    int indent_space_count;
    size_t num_previous_lines;
    char **previous_lines;
    int session_id;
};

typedef struct repl repl_t;

repl_t *make_repl() {
    repl_t *repl = malloc(sizeof(repl_t));
    repl->current_ns = strdup("cljs.user");
    repl->current_prompt = NULL;
    repl->history_path = NULL;
    repl->input = NULL;
    repl->indent_space_count = 0;
    repl->num_previous_lines = 0;
    repl->previous_lines = NULL;
    repl->session_id = 0;
    return repl;
}

void empty_previous_lines(repl_t *repl) {
    int i;
    for (i = 0; i < repl->num_previous_lines; i++) {
        free(repl->previous_lines[i]);
    }
    free(repl->previous_lines);
    repl->num_previous_lines = 0;
    repl->previous_lines = NULL;
}

#define SEC_PROMPT "#_=> "

char *form_prompt(repl_t *repl, bool is_secondary) {
    char *prompt = NULL;
    size_t prompt_min_len = 6; // length of SEC_PROMPT literal
    size_t prefix_min_len = 2; // length of "#_" prefix

    char *current_ns = repl->current_ns;
    bool dumb_terminal = repl->session_id != 0 || config.dumb_terminal;

    if (!is_secondary) {
        if (strlen(current_ns) < prefix_min_len && !config.dumb_terminal) {
            prompt = malloc(prompt_min_len * sizeof(char));
            sprintf(prompt, " %s=> ", current_ns);
        } else {
            prompt = str_concat(current_ns, "=> ");
        }
    } else {
        if (!dumb_terminal) {
            size_t ns_len = strlen(current_ns);
            size_t ns_len_extra = (ns_len < prefix_min_len) ?
                                      0 : ns_len - prefix_min_len;
            prompt = malloc((prompt_min_len + ns_len_extra) * sizeof(char));
            memset(prompt, ' ', ns_len_extra);
            sprintf(prompt + ns_len_extra, SEC_PROMPT);
        }
    }

    return prompt;
}

char *get_input() {
    char *line = NULL;
    size_t len = 0;
    ssize_t n = getline(&line, &len, stdin);
    if (n == -1) { // Ctrl-D
        return NULL;
    }
    if (n > 0) {
        if (line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
    }
    return line;
}

void display_prompt(char *prompt) {
    if (prompt != NULL) {
        fprintf(stdout, "%s", prompt);
        fflush(stdout);
    }
}

bool is_whitespace(char *s) {
    size_t len = strlen(s);
    int i;
    for (i = 0; i < len; i++) {
        if (!isspace(s[i])) {
            return false;
        }
    }

    return true;
}

bool is_exit_command(char *input, bool is_socket_repl) {
    return (strcmp(input, ":cljs/quit") == 0 ||
            strcmp(input, "quit") == 0 ||
            strcmp(input, "exit") == 0 ||
            strcmp(input, "\x04") == 0 ||
            (is_socket_repl && strcmp(input, ":repl/quit") == 0));
}

bool process_line(repl_t *repl, char *input_line, bool split_on_newlines) {

    // Accumulate input lines

    if (repl->input == NULL) {
        repl->input = input_line;
    } else {
        repl->input = realloc(repl->input, (strlen(repl->input) + strlen(input_line) + 2) * sizeof(char));
        sprintf(repl->input + strlen(repl->input), "\n%s", input_line);
    }

    repl->num_previous_lines += 1;
    repl->previous_lines = realloc(repl->previous_lines, repl->num_previous_lines * sizeof(char *));
    repl->previous_lines[repl->num_previous_lines - 1] = strdup(input_line);

    // Check for explicit exit

    if (is_exit_command(repl->input, repl->session_id != 0)) {
        if (repl->session_id == 0) {
            exit(0);
        }
        return true;
    }

    // Add input line to history

    if (repl->history_path != NULL && !is_whitespace(repl->input)) {

        // Split on newlines because input_line will contain newlines if pasting
        if (split_on_newlines) {
            char *tokenize = strdup(input_line);
            char *saveptr = NULL;
            char *token = strtok_r(tokenize, "\n", &saveptr);
            while (token != NULL) {
                linenoiseHistoryAdd(token);
                token = strtok_r(NULL, "\n", &saveptr);
            }
            free(tokenize);
        } else {
            linenoiseHistoryAdd(input_line);
        }

        linenoiseHistorySave(repl->history_path);
    }

    // Check if we now have readable forms
    // and if so, evaluate them

    bool done = false;
    char *balance_text = NULL;

    while (!done) {
        if ((balance_text = is_readable(repl->input)) != NULL) {
            repl->input[strlen(repl->input) - strlen(balance_text)] = '\0';

            if (!is_whitespace(repl->input)) { // Guard against empty string being read

                return_termsize = !config.dumb_terminal;

                if (repl->session_id == 0) {
                    set_int_handler();
                }

                // TODO: set exit value

                const char *theme = repl->session_id == 0 ? config.theme : "dumb";

                evaluate_source("text", repl->input, true, true, repl->current_ns, theme, true,
                                repl->session_id);

                if (repl->session_id == 0) {
                    clear_int_handler();
                }

                return_termsize = false;

                if (exit_value != 0) {
                    free(repl->input);
                    return true;
                }
            } else {
                engine_print("\n");
            }

            // Now that we've evaluated the input, reset for next round
            free(repl->input);
            repl->input = balance_text;

            empty_previous_lines(repl);

            // Fetch the current namespace and use it to set the prompt

            char *current_ns = get_current_ns();
            if (current_ns) {
                free(repl->current_ns);
                repl->current_ns = current_ns;
                free(repl->current_prompt);
                repl->current_prompt = form_prompt(repl, false);
            }

            if (is_whitespace(balance_text)) {
                done = true;
                free(repl->input);
                repl->input = NULL;
            }
        } else {
            // Prepare for reading non-1st of input with secondary prompt
            if (repl->history_path != NULL) {
                if (!is_pasting()) {
                    repl->indent_space_count = indent_space_count(repl->input);
                }
            }

            free(repl->current_prompt);
            repl->current_prompt = form_prompt(repl, true);
            done = true;
        }
    }

    return false;
}

pthread_mutex_t repl_print_mutex = PTHREAD_MUTEX_INITIALIZER;

void run_cmdline_loop(repl_t *repl) {

    char *input_line = NULL;

    while (true) {

        /* Gross hack to avoid a race condition. If
         * evaluating (js/setTimeout #(prn 1) 0)
         * sometimes the (prn 1) side effect does not
         * appear. Sleeping a millisecond here
         * appears to successfully work around
         * whatever is causing it.
         */
        struct timespec t;
        t.tv_sec = 0;
        t.tv_nsec = 1000 * 1000;
        int err;
        while ((err = nanosleep(&t, &t)) && errno == EINTR) {}
        if (err) {
            engine_perror("repl nanosleep");
        }

        if (config.dumb_terminal) {
            display_prompt(repl->current_prompt);
            free(input_line);
            input_line = get_input();
            if (input_line == NULL) { // Ctrl-D pressed
                printf("\n");
                break;
            }
            pthread_mutex_lock(&repl_print_mutex);
        } else {
            // Handle prints while processing linenoise input
            bool linenoisePrintNowSet = false;
            if (engine_ready) {
                pthread_mutex_lock(&repl_print_mutex);
                set_print_sender(&linenoisePrintNow);
                pthread_mutex_unlock(&repl_print_mutex);
                linenoisePrintNowSet = true;
            }

            // If *print-newline* is off, we need to emit a newline now, otherwise
            // the linenoise prompt and line editing will overwrite any printed
            // output on the current line.
            if (engine_ready && !engine_print_newline()) {
                fprintf(stdout, "\n");
            }

            char *secondary_prompt = form_prompt(repl, true);
            char *line = linenoise(repl->current_prompt, secondary_prompt, prompt_ansi_code_for_theme(config.theme),
                                   repl->indent_space_count);
            free(secondary_prompt);

            pthread_mutex_lock(&repl_print_mutex);

            // Reset printing handler back
            if (linenoisePrintNowSet) {
                set_print_sender(NULL);
            }

            if (line == NULL) {
                if (errno == EAGAIN) { // Ctrl-C
                    errno = 0;
                    repl->input = NULL;
                    repl->indent_space_count = 0;
                    empty_previous_lines(repl);
                    free(repl->current_prompt);
                    repl->current_prompt = form_prompt(repl, false);
                    printf("\n");
                    pthread_mutex_unlock(&repl_print_mutex);
                    continue;
                } else { // Ctrl-D
                    exit_value = 0;
                    pthread_mutex_unlock(&repl_print_mutex);
                    break;
                }
            }

            free(input_line);
            input_line = line;
        }

        // If the input is small process each line separately here
        // so that things like brace highlighting work properly.
        // But for large input, let process_line() more efficiently
        // handle the input. The initial case is for a new line (the
        // new itself is not part of input_line).
        bool break_out = false;
        if (repl->input != NULL && strlen(input_line) == 0) {
            repl->indent_space_count = 0;
            break_out = process_line(repl, input_line, false);
        } else if (strlen(input_line) < 16384) {
            char *tokenize = strdup(input_line);
            char *saveptr = NULL;
            char *token = strtok_r(tokenize, "\n", &saveptr);
            while (token != NULL) {
                repl->indent_space_count = 0;
                break_out = process_line(repl, strdup(token), false);
                if (break_out) {
                    break;
                }
                token = strtok_r(NULL, "\n", &saveptr);
            }
            free(tokenize);
        } else {
            repl->indent_space_count = 0;
            break_out = process_line(repl, input_line, true);
        }

        pthread_mutex_unlock(&repl_print_mutex);
        if (break_out) {
            break;
        }
    }

    free(input_line);
}

void completion(const char *buf, linenoiseCompletions *lc) {
    int num_completions = 0;
    char **completions = get_completions(buf, &num_completions);

    if (completions) {
        int i;
        for (i = 0; i < num_completions; i++) {
            linenoiseAddCompletion(lc, completions[i]);
            free(completions[i]);
        }
        free(completions);
    }
}

pthread_mutex_t highlight_restore_sequence_mutex = PTHREAD_MUTEX_INITIALIZER;
int highlight_restore_sequence = 0;

struct hl_restore {
    int id;
    int num_lines_up;
    int relative_horiz;
};

struct hl_restore hl_restore = {0, 0, 0};

void do_highlight_restore(void *data) {

    struct hl_restore *hl_restore = data;

    int highlight_restore_sequence_value;
    pthread_mutex_lock(&highlight_restore_sequence_mutex);
    highlight_restore_sequence_value = highlight_restore_sequence;
    pthread_mutex_unlock(&highlight_restore_sequence_mutex);

    if (hl_restore->id == highlight_restore_sequence_value) {

        pthread_mutex_lock(&highlight_restore_sequence_mutex);
        ++highlight_restore_sequence;
        pthread_mutex_unlock(&highlight_restore_sequence_mutex);

        if (hl_restore->num_lines_up != 0) {
            fprintf(stdout, "\x1b[%dB", hl_restore->num_lines_up);
        }

        if (hl_restore->relative_horiz < 0) {
            fprintf(stdout, "\x1b[%dC", -hl_restore->relative_horiz);
        } else if (hl_restore->relative_horiz > 0) {
            fprintf(stdout, "\x1b[%dD", hl_restore->relative_horiz);
        }

        fflush(stdout);

    }

    free(hl_restore);
}

// Used when using linenoise
repl_t *s_repl;

void highlight(const char *buf, int pos) {
    char current = buf[pos];

    if (current == ']' || current == '}' || current == ')') {
        int num_lines_up = -1;
        int highlight_pos = 0;
        highlight_coords_for_pos(pos, buf, s_repl->num_previous_lines, s_repl->previous_lines,
                                 &num_lines_up,
                                 &highlight_pos);

        int current_pos = pos + 1;

        if (num_lines_up != -1) {
            int relative_horiz = highlight_pos - current_pos;

            struct winsize w;
            int rv = ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
            int terminal_width = rv == -1 ? 80 : w.ws_col;

            int prompt_length = (int) strlen(s_repl->current_prompt);

            int cursor_absolute_pos = current_pos + prompt_length + 1;
            int highlight_absolute_pos = highlight_pos + prompt_length;

            if (cursor_absolute_pos > terminal_width
                && -relative_horiz >= cursor_absolute_pos % terminal_width) {
                relative_horiz = -(-relative_horiz - terminal_width) % terminal_width;
                num_lines_up += 1 + (terminal_width * (cursor_absolute_pos / terminal_width)
                                     - highlight_absolute_pos) / terminal_width;
            }

            // The math above isn't correct for very large buffered lines, so
            // simply skip hopping in that case to avoid botching the terminal.
            if (cursor_absolute_pos > (3 * terminal_width - prompt_length)) {
                return;
            }

            if (num_lines_up != 0) {
                fprintf(stdout, "\x1b[%dA", num_lines_up);
            }

            if (relative_horiz < 0) {
                fprintf(stdout, "\x1b[%dD", -relative_horiz);
            } else if (relative_horiz > 0) {
                fprintf(stdout, "\x1b[%dC", relative_horiz);
            }

            fflush(stdout);

            struct hl_restore *hl_restore_local = malloc(sizeof(struct hl_restore));
            pthread_mutex_lock(&highlight_restore_sequence_mutex);
            hl_restore_local->id = ++highlight_restore_sequence;
            pthread_mutex_unlock(&highlight_restore_sequence_mutex);
            hl_restore_local->num_lines_up = num_lines_up;
            hl_restore_local->relative_horiz = relative_horiz;

            hl_restore = *hl_restore_local;

            start_timer(500, do_highlight_restore, (void *) hl_restore_local);
        }
    }
}

void highlight_cancel() {
    if (hl_restore.id != 0) {
        struct hl_restore *hl_restore_tmp = malloc(sizeof(struct hl_restore));
        *hl_restore_tmp = hl_restore;
        do_highlight_restore(hl_restore_tmp);
    }
}

int sock_to_write_to = 0;

void socket_sender(const char *text) {
    if (sock_to_write_to) {
        write_to_socket(sock_to_write_to, text);
    }
}

static int session_id_counter = 0;

conn_data_cb_ret_t* socket_repl_data_arrived(char *data, int sock, void *state) {

    int err = 0;
    bool exit = false;

    if (data) {
        repl_t *repl = state;

        if (str_has_suffix(data, "\r\n") == 0) {
            data[strlen(data) - 2] = '\0';
        }

        sock_to_write_to = sock;

        pthread_mutex_lock(&repl_print_mutex);

        set_print_sender(&socket_sender);

        exit = process_line(repl, strdup(data), false);

        set_print_sender(NULL);
        sock_to_write_to = 0;

        pthread_mutex_unlock(&repl_print_mutex);


        if (!exit && repl->current_prompt != NULL) {
            err = write_to_socket(sock, repl->current_prompt);
        }
    } else {
        exit = true;
    }

    conn_data_cb_ret_t* connection_data_arrived_return = malloc(sizeof(conn_data_cb_ret_t));

    connection_data_arrived_return->err = err;
    connection_data_arrived_return->close = exit;

    return connection_data_arrived_return;
}

accepted_conn_cb_ret_t* accepted_socket_repl_connection(int sock, void* state) {
    repl_t *repl = make_repl();
    repl->current_prompt = form_prompt(repl, false);
    repl->session_id = ++session_id_counter;

    int err = write_to_socket(sock, repl->current_prompt);

    accepted_conn_cb_ret_t* accepted_connection_cb_return = malloc(sizeof(accepted_conn_cb_ret_t));

    accepted_connection_cb_return->err = err;
    accepted_connection_cb_return->info = repl;

    return accepted_connection_cb_return;
}

void socket_repl_listen_successful_cb() {
    if (!config.quiet) {
        char msg[1024];
        snprintf(msg, 1024, "Planck socket REPL listening at %s:%d.\n", config.socket_repl_host,
                 config.socket_repl_port);
        engine_print(msg);
    }
}

int run_repl() {
    repl_t *repl = make_repl();
    s_repl = repl;

    repl->current_prompt = form_prompt(repl, false);

    // Per-type initialization

    if (!config.dumb_terminal) {

        linenoiseSetupSigWinchHandler();

        char *home = getenv("HOME");
        if (home != NULL) {
            char history_name[] = ".planck_history";
            size_t len = strlen(home) + strlen(history_name) + 2;
            repl->history_path = malloc(len * sizeof(char));
            snprintf(repl->history_path, len, "%s/%s", home, history_name);

            linenoiseHistoryLoad(repl->history_path);

            exit_value = load_keymap(home);
            if (exit_value != EXIT_SUCCESS) {
                return exit_value;
            }
        }

        linenoiseSetMultiLine(1);
        linenoiseSetCompletionCallback(completion);
        linenoiseSetHighlightCallback(highlight);
        linenoiseSetHighlightCancelCallback(highlight_cancel);
    }

    socket_accept_info_t socket_accept_data = {config.socket_repl_host,
                                               config.socket_repl_port,
                                               socket_repl_listen_successful_cb,
                                               accepted_socket_repl_connection,
                                               socket_repl_data_arrived,
                                               0,
                                               NULL};

    if (config.socket_repl_port) {
        block_until_engine_ready();

        if (config.dumb_terminal) {
            set_print_sender(NULL);
        } else {
            set_print_sender(&linenoisePrintNow);
        }

        int err = bind_and_listen(&socket_accept_data);
        if (err == -1) {
            engine_perror("Failed to set up socket REPL");
        } else {
            pthread_t thread;
            pthread_create(&thread, NULL, accept_connections, &socket_accept_data);
        }
    }

    run_cmdline_loop(repl);

    return exit_value;
}
