#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_COMMAND_LINE_LEN 1024
#define MAX_COMMAND_LINE_ARGS 128

char prompt[] = "> ";
char delimiters[] = " \t\r\n";
extern char **environ;

pid_t foreground_pid = -1;

void sigint_handler(int sig) {
    if (foreground_pid > 0) {
        kill(foreground_pid, SIGINT);
    } else {
        printf("\n");
    }
}

void sigalrm_handler(int sig) {
    if (foreground_pid > 0) {
        printf("\nProcess exceeded time limit (10 seconds). Terminating...\n");
        kill(foreground_pid, SIGKILL);
    }
}

int main() {
    signal(SIGINT, sigint_handler);
    signal(SIGALRM, sigalrm_handler);

    char command_line[MAX_COMMAND_LINE_LEN];
    char *arguments[MAX_COMMAND_LINE_ARGS];
    char cwd[1024];

    while (true) {
        do {
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("%s> ", cwd);
            } else {
                perror("getcwd() error");
                printf("> ");
            }
            fflush(stdout);

            if ((fgets(command_line, MAX_COMMAND_LINE_LEN, stdin) == NULL) && ferror(stdin)) {
                fprintf(stderr, "fgets error");
                exit(0);
            }

        } while(command_line[0] == 0x0A);

        if (feof(stdin)) {
            printf("\n");
            fflush(stdout);
            fflush(stderr);
            return 0;
        }

        if (command_line[0] == '\n') {
            continue;
        }

        int i = 0;
        char *token = strtok(command_line, delimiters);
        while (token != NULL) {
            arguments[i++] = token;
            token = strtok(NULL, delimiters);
        }
        arguments[i] = NULL;

        bool is_background = false;
        if (i > 0 && strcmp(arguments[i-1], "&") == 0) {
            is_background = true;
            arguments[i-1] = NULL;
        }

        // Check for input, output redirection, and pipe
        char *output_file = NULL;
        char *input_file = NULL;
        char *left_cmd[MAX_COMMAND_LINE_ARGS];
        char *right_cmd[MAX_COMMAND_LINE_ARGS];
        int j = 0;
        bool is_pipe = false;

        for (j = 0; j < i; j++) {
            if (strcmp(arguments[j], ">") == 0) {
                arguments[j] = NULL;
                output_file = arguments[j + 1];  // File name for output redirection
                break;
            } else if (strcmp(arguments[j], "<") == 0) {
                arguments[j] = NULL;
                input_file = arguments[j + 1];   // File name for input redirection
                break;
            } else if (strcmp(arguments[j], "|") == 0) {
                // Split the command at the pipe into two parts
                arguments[j] = NULL;
                int k;
                for (k = 0; k < j; k++) {
                    left_cmd[k] = arguments[k];
                }
                left_cmd[j] = NULL;

                int l;
                for (l = 0; arguments[j + 1 + l] != NULL; l++) {
                    right_cmd[l] = arguments[j + 1 + l];
                }
                right_cmd[l] = NULL;
                is_pipe = true;
                break;
            }
        }

        if (strcmp(arguments[0], "cd") == 0) {
            if (arguments[1] == NULL) {
                fprintf(stderr, "cd: expected argument to 'cd'\n");
            } else {
                if (chdir(arguments[1]) != 0) {
                    perror("cd failed");
                }
            }
            continue;
        }

        if (strcmp(arguments[0], "pwd") == 0) {
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("%s\n", cwd);
            } else {
                perror("pwd failed");
            }
            continue;
        }

        if (strcmp(arguments[0], "setenv") == 0) {
            if (arguments[1] == NULL || arguments[2] == NULL) {
                fprintf(stderr, "setenv: expected variable and value\n");
            } else {
                if (setenv(arguments[1], arguments[2], 1) != 0) {
                    perror("setenv failed");
                }
            }
            continue;
        }

        if (strcmp(arguments[0], "env") == 0) {
            char **env = environ;
            while (*env) {
                printf("%s\n", *env++);
            }
            continue;
        }

        if (strcmp(arguments[0], "echo") == 0) {
            int j;
            for (j = 1; arguments[j] != NULL; j++) {
                if (arguments[j][0] == '$') {
                    char *env_var = getenv(arguments[j] + 1);
                    if (env_var) {
                        printf("%s ", env_var);
                    } else {
                        printf(" ");
                    }
                } else {
                    printf("%s ", arguments[j]);
                }
            }
            printf("\n");
            continue;
        }

        if (strcmp(arguments[0], "exit") == 0) {
            exit(0);
        }

        // Handle piping
        if (is_pipe) {
            int pipe_fd[2];
            if (pipe(pipe_fd) == -1) {
                perror("pipe failed");
                continue;
            }

            pid_t left_pid = fork();
            if (left_pid == 0) {
                // Left-hand command (writing to the pipe)
                close(pipe_fd[0]);  // Close unused read end

                // Input redirection for the left-hand command
                if (input_file != NULL) {
                    int fd = open(input_file, O_RDONLY);
                    if (fd < 0) {
                        perror("Failed to open file for input redirection");
                        exit(EXIT_FAILURE);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }

                // Redirect stdout to pipe
                dup2(pipe_fd[1], STDOUT_FILENO);
                close(pipe_fd[1]);

                if (execvp(left_cmd[0], left_cmd) == -1) {
                    perror("execvp failed for left-hand command");
                    exit(EXIT_FAILURE);
                }
            }

            pid_t right_pid = fork();
            if (right_pid == 0) {
                // Right-hand command (reading from the pipe)
                close(pipe_fd[1]);  // Close unused write end

                // Output redirection for the right-hand command
                if (output_file != NULL) {
                    int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd < 0) {
                        perror("Failed to open file for output redirection");
                        exit(EXIT_FAILURE);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }

                // Redirect stdin to pipe
                dup2(pipe_fd[0], STDIN_FILENO);
                close(pipe_fd[0]);

                if (execvp(right_cmd[0], right_cmd) == -1) {
                    perror("execvp failed for right-hand command");
                    exit(EXIT_FAILURE);
                }
            }

            // Parent process
            close(pipe_fd[0]);
            close(pipe_fd[1]);

            waitpid(left_pid, NULL, 0);
            waitpid(right_pid, NULL, 0);

        } else {
            // No pipe, regular command execution
            pid_t pid = fork();
            if (pid == 0) {
                // Child process

                if (input_file != NULL) {
                    // Open file for input redirection
                    int fd = open(input_file, O_RDONLY);
                    if (fd < 0) {
                        perror("Failed to open file for input redirection");
                        exit(EXIT_FAILURE);
                    }

                    // Redirect stdin to the file
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }

                if (output_file != NULL) {
                    // Open file for output redirection
                    int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd < 0) {
                        perror("Failed to open file for output redirection");
                        exit(EXIT_FAILURE);
                    }

                    // Redirect stdout to the file
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }

                if (execvp(arguments[0], arguments) == -1) {
                    perror("execvp failed");
                }
                exit(EXIT_FAILURE);
            } else if (pid < 0) {
                perror("fork failed");
            } else {
                if (!is_background) {
                    foreground_pid = pid;
                    alarm(10);
                    wait(NULL);
                    alarm(0);
                    foreground_pid = -1;
                }
            }
        }
    }

    return 0;
}