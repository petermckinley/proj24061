#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"
#include "swish_funcs.h"

#define CMD_LEN 512
#define PROMPT "@> "

int main(int argc, char **argv) {
  struct sigaction sac;
  sac.sa_handler = SIG_IGN;
  if (sigfillset(&sac.sa_mask) == -1) {
    perror("sigfillset");
    return 1;
  }
  sac.sa_flags = 0;
  if (sigaction(SIGTTIN, &sac, NULL) == -1 ||
      sigaction(SIGTTOU, &sac, NULL) == -1) {
    perror("sigaction");
    return 1;
  }

  strvec_t tokens;
  strvec_init(&tokens);
  job_list_t jobs;
  job_list_init(&jobs);
  char cmd[CMD_LEN];

  printf("%s", PROMPT);
  while (fgets(cmd, CMD_LEN, stdin) != NULL) {
    // Need to remove trailing '\n' from cmd. There are fancier ways.
    int i = 0;
    while (cmd[i] != '\n') {
      i++;
    }
    cmd[i] = '\0';

    if (tokenize(cmd, &tokens) != 0) {
      printf("Failed to parse command\n");
      strvec_clear(&tokens);
      job_list_free(&jobs);
      return 1;
    }
    if (tokens.length == 0) {
      printf("%s", PROMPT);
      continue;
    }
    const char *first_token = strvec_get(&tokens, 0);

    if (strcmp(first_token, "pwd") == 0) {
      char buf[BUFSIZ];
      if (getcwd(buf, BUFSIZ) == NULL) {
        strvec_clear(&tokens);
        job_list_free(&jobs);
        return 1;
      }
      printf("%s\n", buf);
    }

    else if (strcmp(first_token, "cd") == 0) {
      char *second_token = strvec_get(&tokens, 1);
      if (second_token == NULL) {
        chdir(getenv("HOME"));
      } else if (chdir(second_token) == -1) {
        perror("chdir");
      }
    }

    else if (strcmp(first_token, "exit") == 0) {
      strvec_clear(&tokens);
      break;
    }

    else if (strcmp(first_token, "jobs") == 0) {
      int i = 0;
      job_t *current = jobs.head;
      while (current != NULL) {
        char *status_desc;
        if (current->status == BACKGROUND) {
          status_desc = "background";
        } else {
          status_desc = "stopped";
        }
        printf("%d: %s (%s)\n", i, current->name, status_desc);
        i++;
        current = current->next;
      }
    }

    else if (strcmp(first_token, "fg") == 0) {

      if (resume_job(&tokens, &jobs, 1) == 1) {
        printf("Failed to resume job in foreground\n");
      }
    }

    else if (strcmp(first_token, "bg") == 0) {
      if (resume_job(&tokens, &jobs, 0) == 1) {
        printf("Failed to resume job in background\n");
      }
    }

    else if (strcmp(first_token, "wait-for") == 0) {
      if (await_background_job(&tokens, &jobs) == -1) {
        printf("Failed to wait for background job\n");
      }
    }

    else if (strcmp(first_token, "wait-all") == 0) {
      if (await_all_background_jobs(&jobs) == -1) {
        printf("Failed to wait for all background jobs\n");
      }
    }

    else {
      char *cmd_name = strvec_get(&tokens, 0);

      int is_background = 0;
      if (tokens.length > 0) {
        const char *last_token = strvec_get(&tokens, tokens.length - 1);
        if (strcmp(last_token, "&") == 0) {
          is_background = 1;
          char *removed_ampersand = tokens.data[tokens.length - 1];
          free(removed_ampersand);
          tokens.length--;
        }
      }

      pid_t pid = fork();

      if (pid < 0) {
        perror("pid");
      } else if (pid == 0) {
        pid_t cpid = getpid();
        if (setpgid(cpid, cpid) == -1) {
          perror("setpgid (child)");
          return 1;
        }

        if (run_command(&tokens)) {
          return 1;
        }
      } else {
        if (setpgid(pid, pid) == -1) {
          perror("setpgid (parent)");
        }

        if (is_background) {
          job_list_add(&jobs, pid, cmd_name, BACKGROUND);

        } else {

          if (tcsetpgrp(STDIN_FILENO, pid) == -1) {
            perror("tcsetpgrp (to child)");
          }

          int status;
          waitpid(pid, &status, WUNTRACED);

          if (WIFSTOPPED(status)) {
            job_list_add(&jobs, pid, cmd_name, STOPPED);
          }

          if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1) {
            perror("tcsetpgrp (to shell)");
          }
        }
      }
    }
    strvec_clear(&tokens);
    printf("%s", PROMPT);
  }

  job_list_free(&jobs);
  return 0;
} // AAAAAHHHH
