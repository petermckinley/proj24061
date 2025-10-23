#define _GNU_SOURCE

#include "swish_funcs.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"

#define MAX_ARGS 10

/*
 * splits input string by spaces, stores in array
 */
int tokenize(char *s, strvec_t *tokens) {
  char *buf = strtok(s, " ");

  while (buf != NULL) {
    strvec_add(tokens, buf);
    buf = strtok(NULL, " ");
  }
  return 0;
}

/*
runs external, non supported commands, execvp
*/
int run_command(strvec_t *tokens) {
  int count = tokens->length;
  char *argv[count + 1];

  for (int i = 0; i < count; i++) {
    argv[i] = strvec_get(tokens, i);
  }

  argv[count] = NULL;

  int in_index = strvec_find(tokens, "<");
  int out_index = strvec_find(tokens, ">");
  int append_index = strvec_find(tokens, ">>");

  // input redirctions stuff!
  if (in_index != -1) {
    char *infile = strvec_get(tokens, in_index + 1);
    if (infile == NULL) {
      fprintf(stderr, "Error: missing input file for '<'\n");
      return 1;
    }
    int fd = open(infile, O_RDONLY);
    if (fd < 0) {
      perror("Failed to open input file");
      return 1;
    }

    if (dup2(fd, STDIN_FILENO) < 0) {
      perror("dup2 input");
      close(fd);
      return 1;
    }

    close(fd);
    // remove from argv
    argv[in_index] = NULL;
  }

  // output redirection
  if (out_index != -1) {
    char *outfile = strvec_get(tokens, out_index + 1);
    if (outfile == NULL) {
      fprintf(stderr, "error: missing output file for '>'\n");
      return 1;
    }

    int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      perror("Failed to open output file");
      return 1;
    }

    if (dup2(fd, STDOUT_FILENO) < 0) {
      perror("dup2 output");
      close(fd);
      return 1;
    }

    close(fd);
    argv[out_index] = NULL;
  }

  // append redirection stuff
  if (append_index != -1) {
    char *outfile = strvec_get(tokens, append_index + 1);
    if (outfile == NULL) {
      fprintf(stderr, "error: missing output file for '>>'\n");
      return 1;
    }

    int fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
      perror("open append");
      return 1;
    }

    if (dup2(fd, STDOUT_FILENO) < 0) {
      perror("dup2 append");
      close(fd);
      return 1;
    }

    close(fd);
    argv[append_index] = NULL;
  }

  // restore defualt behavior for stdin stuff
  struct sigaction sac;
  sac.sa_handler = SIG_DFL;

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

  execvp(argv[0], argv);
  perror("exec");
  return 1;
}

/*
restarts ended job, give terminal control to it
*/
int resume_job(strvec_t *tokens, job_list_t *jobs, int is_foreground) {
  if (tokens->length < 2) {
    fprintf(stderr,
            "resume: missing job index\n"); // perror here returns sucess too,
                                            // this seemed to work
    return 1;
  }

  int job_index = atoi(strvec_get(tokens, 1));
  job_t *job = job_list_get(jobs, job_index);
  if (job == NULL) {
    fprintf(stderr, "Job index out of bounds\n"); // same down here
    return 1;
  }

  pid_t job_pid = job->pid;
  pid_t shell_pid = getpid();

  if (is_foreground) {
    if (tcsetpgrp(STDIN_FILENO, job_pid) == -1) {
      perror("tcsetpgrp (to job)");
      return 1;
    }
  }

  if (kill(-job_pid, SIGCONT) == -1) {
    perror("kill(SIGCONT)");
    return 1;
  }

  if (is_foreground) {
    int status;
    pid_t wpid = waitpid(job_pid, &status, WUNTRACED);

    if (wpid == -1) {
      perror("waitpid");
    } else {
      if (WIFSTOPPED(status)) {
        job->status = STOPPED;
      } else {
        job_list_remove(jobs, job_index);
      }
    }

    if (tcsetpgrp(STDIN_FILENO, shell_pid) == -1) {
      perror("tcsetpgrp (to shell)");
    }
  } else {
    job->status = BACKGROUND;
  }

  return 0;
}

// waits for background jobs to finish
int await_background_job(strvec_t *tokens, job_list_t *jobs) {
  if (tokens->length < 2) {
    fprintf(stderr, "await: missing job index\n");
    return 1;
  }

  int job_index = atoi(strvec_get(tokens, 1));
  job_t *job = job_list_get(jobs, job_index);

  if (job == NULL) {
    fprintf(stderr, "Job index out of bounds\n");
    return 1;
  }

  if (job->status != BACKGROUND) {
    printf("Job index is for stopped process not background process \nFailed "
           "to wait for background job\n");
    return 1;
  }

  int status;
  pid_t wpid = waitpid(job->pid, &status, WUNTRACED);

  if (wpid == -1) {
    perror("waitpid");
    return 1;
  }

  if (WIFSTOPPED(status)) {
    job->status = STOPPED;
    printf("\nJob [%d] %d stopped: %s\n", job_index, job->pid, job->name);
  } else {
    job_list_remove(jobs, job_index);
  }
  return 0;
}

// does it more!
int await_all_background_jobs(job_list_t *jobs) {
  job_t *current = jobs->head;
  int job_index_counter = 0;

  while (current != NULL) {
    if (current->status != BACKGROUND) {
      current = current->next;
      job_index_counter++;
      continue;
    }

    int status;
    pid_t wpid = waitpid(current->pid, &status, WUNTRACED);

    if (wpid == -1) {
      perror("waitpid");
      // If waitpid fails, try the next job
      current = current->next;
      job_index_counter++;
      continue;
    }
    if (WIFSTOPPED(status)) {
      current->status = STOPPED;
      printf("\nJob [%d] %d stopped: %s\n", job_index_counter, current->pid,
             current->name);
      current = current->next;
    }
    current = current->next;
    job_index_counter++;
  }
  job_list_remove_by_status(jobs, BACKGROUND);

  return 0;
}
