/*
 * pty_engine.c — Phase 1: PTY Engine
 *
 * Spawns a child shell over a pseudo-terminal and captures its raw
 * byte output, writing it straight to stdout.  No user input is
 * handled in this phase.
 *
 * Compile:
 *   gcc -Wall -Wextra -o pty_engine pty_engine.c -lutil
 *
 * Run:
 *   ./pty_engine
 */

#include <pty.h>      /* forkpty()          */
#include <stdio.h>    /* perror()           */
#include <stdlib.h>   /* exit(), EXIT_*     */
#include <sys/wait.h> /* waitpid()          */
#include <unistd.h>   /* read(), write(), execlp(), STDOUT_FILENO */

#define BUF_SIZE 4096 /* read buffer — one page */

int main(void) {
  int master_fd; /* master side of the PTY pair */
  pid_t pid;

  /*
   * forkpty() does three things in one call:
   *   1. Opens a new PTY master/slave pair.
   *   2. Forks the process.
   *   3. In the child, makes the slave the controlling terminal
   *      and dups it onto stdin/stdout/stderr.
   *
   * Returns:
   *   - child PID to the parent  (master_fd is set)
   *   - 0 to the child
   *   - -1 on failure
   */
  pid = forkpty(&master_fd, NULL, NULL, NULL);

  if (pid < 0) {
    perror("forkpty");
    exit(EXIT_FAILURE);
  }

  /* ── Child process ─────────────────────────────────────────── */
  if (pid == 0) {
    /*
     * Replace this process image with a shell.
     * execlp searches $PATH, so "bash" is enough.
     * The shell sees a real PTY as its terminal and will
     * emit prompts, color codes, etc.
     */
    execlp("/bin/bash", "bash", (char *)NULL);

    /* If we get here, exec failed */
    perror("execlp");
    _exit(EXIT_FAILURE); /* _exit in child after fork */
  }

  /* ── Parent process ────────────────────────────────────────── */
  {
    char buf[BUF_SIZE];
    ssize_t n;

    /*
     * Sit in a tight loop reading whatever the child shell
     * writes to its stdout/stderr (which goes through the
     * PTY slave → PTY master).  Mirror it to our own stdout
     * so we can see the raw output.
     */
    for (;;) {
      n = read(master_fd, buf, sizeof(buf));

      if (n <= 0) {
        /*
         * n == 0 → EOF, slave side closed (shell exited).
         * n <  0 → read error (EIO is normal when slave
         *          hangs up on some systems).
         * Either way, we're done.
         */
        break;
      }

      /* Write exactly what we read — no buffering, no mangling */
      if (write(STDOUT_FILENO, buf, (size_t)n) < 0) {
        perror("write");
        break;
      }
    }

    /* Reap the child so it doesn't become a zombie */
    waitpid(pid, NULL, 0);

    /* Close the master fd */
    close(master_fd);
  }

  return EXIT_SUCCESS;
}
