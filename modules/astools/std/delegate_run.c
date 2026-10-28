/*
 * delegate_run.c — supervised streaming runner for the delegate tool.
 *
 * Unlike astd_run_capture (which buffers whole streams up to a cap), an
 * agent CLI may run for half an hour and print megabytes of events whose
 * last line is the one that matters. This runner hands every complete
 * stdout line to a callback and keeps only the line being assembled
 * (bounded by max_line) and a fixed-size stderr tail.
 *
 * The child stays in the caller's process group: if the runtime cancels
 * the invocation it kills the whole group, CLI and descendants included.
 * At the local deadline the runner sends SIGTERM to the CLI (both CLIs
 * stop their own subprocesses on it), then SIGKILL after a grace period.
 * Once the CLI has exited, descendants that still hold its pipes get a
 * short drain window; the runtime reaps whatever remains.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "delegate.h"
#include "sdk.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

void dlg_run_res_free(dlg_run_res *res) {
  if (!res) return;
  free(res->stderr_tail);
  res->stderr_tail = NULL;
  res->stderr_tail_n = 0;
}

#ifdef _WIN32

int dlg_run(const dlg_run_opts *o, dlg_run_res *res, char *emsg,
            size_t emsg_sz) {
  (void)o;
  memset(res, 0, sizeof *res);
  res->exit_code = -1;
  snprintf(emsg, emsg_sz, "delegates are not supported on Windows");
  return -1;
}

#else /* POSIX */

#define DLG_DRAIN_MS 2000 /* pipe holders after the CLI exited */
#define DLG_TICK_MS 100

typedef struct {
  char *buf;
  size_t n, cap;
  int skipping; /* inside a line longer than max_line */
} line_buf;

static void close_fd(int *fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static int set_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFD);
  return fl < 0 ? -1 : fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL);
  return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Child side: report the failing stage and errno, then exit. */
static void child_report(int fd, char stage) {
  char msg[1 + sizeof(int)];
  int e = errno;
  msg[0] = stage;
  memcpy(msg + 1, &e, sizeof e);
  if (write(fd, msg, sizeof msg) < 0) { /* nothing left to do */
  }
  _exit(127);
}

static int lb_append(line_buf *lb, const char *data, size_t n,
                     size_t max_line) {
  if (lb->n + n + 1 > lb->cap) {
    size_t ncap = lb->cap ? lb->cap : 4096;
    char *p;
    while (ncap < lb->n + n + 1) ncap *= 2;
    if (ncap > max_line + 1) ncap = max_line + 1;
    p = realloc(lb->buf, ncap);
    if (!p) return -1;
    lb->buf = p;
    lb->cap = ncap;
  }
  memcpy(lb->buf + lb->n, data, n);
  lb->n += n;
  lb->buf[lb->n] = '\0';
  return 0;
}

static void dispatch(const dlg_run_opts *o, dlg_run_res *res, line_buf *lb) {
  res->lines++;
  if (o->on_line) o->on_line(o->ud, lb->buf ? lb->buf : "", lb->n);
}

static void feed_stdout(const dlg_run_opts *o, dlg_run_res *res,
                        line_buf *lb, const char *data, size_t n) {
  size_t i = 0;
  res->stdout_bytes += n;
  while (i < n) {
    const char *nl = memchr(data + i, '\n', n - i);
    size_t seg = nl ? (size_t)(nl - (data + i)) : n - i;
    if (!lb->skipping && seg > 0) {
      /* Too long (or no memory to hold it): skip to the next newline. */
      if (lb->n + seg > o->max_line ||
          lb_append(lb, data + i, seg, o->max_line) != 0) {
        lb->skipping = 1;
        lb->n = 0;
        res->oversized_lines++;
      }
    }
    i += seg;
    if (nl) {
      if (!lb->skipping) dispatch(o, res, lb);
      lb->n = 0;
      lb->skipping = 0;
      i++;
    }
  }
}

static void feed_stderr(dlg_run_res *res, size_t cap, const char *data,
                        size_t n) {
  char *tail = res->stderr_tail;
  res->stderr_bytes += n;
  if (cap == 0) return;
  if (n >= cap) {
    memcpy(tail, data + (n - cap), cap);
    res->stderr_tail_n = cap;
  } else {
    if (res->stderr_tail_n + n > cap) {
      size_t drop = res->stderr_tail_n + n - cap;
      memmove(tail, tail + drop, res->stderr_tail_n - drop);
      res->stderr_tail_n -= drop;
    }
    memcpy(tail + res->stderr_tail_n, data, n);
    res->stderr_tail_n += n;
  }
  tail[res->stderr_tail_n] = '\0';
}

/* One read from a ready stream; closes it at EOF or on a hard error. */
static void pump(const dlg_run_opts *o, dlg_run_res *res, line_buf *lb,
                 int *fd, int is_stdout) {
  char buf[65536];
  ssize_t n = read(*fd, buf, sizeof buf);
  if (n > 0) {
    if (is_stdout)
      feed_stdout(o, res, lb, buf, (size_t)n);
    else
      feed_stderr(res, o->stderr_cap, buf, (size_t)n);
  } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
    close_fd(fd);
  }
}

int dlg_run(const dlg_run_opts *o, dlg_run_res *res, char *emsg,
            size_t emsg_sz) {
  int inp[2] = {-1, -1}, outp[2] = {-1, -1}, errp[2] = {-1, -1};
  int exep[2] = {-1, -1};
  int st = 0, reaped = 0, term_sent = 0, kill_sent = 0, no_status = 0;
  size_t inoff = 0;
  pid_t pid = -1;
  line_buf lb;
  int64_t t0 = 0, now = 0, deadline = 0, kill_at = 0, drain_until = 0;
  struct sigaction ign, old_pipe;
  int rc_out = -1;

  memset(res, 0, sizeof *res);
  memset(&lb, 0, sizeof lb);
  res->exit_code = -1;
  if (emsg_sz) emsg[0] = '\0';
  if (!o || !o->argv || !o->argv[0] || !o->envp) {
    snprintf(emsg, emsg_sz, "invalid runner options");
    return -1;
  }
  res->stderr_tail = malloc(o->stderr_cap + 1);
  if (!res->stderr_tail) {
    snprintf(emsg, emsg_sz, "out of memory");
    return -1;
  }
  res->stderr_tail[0] = '\0';
  /* A CLI that stops reading its stdin must not kill this process. */
  memset(&ign, 0, sizeof ign);
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  if (sigaction(SIGPIPE, &ign, &old_pipe) != 0) {
    snprintf(emsg, emsg_sz, "sigaction: %s", strerror(errno));
    return -1;
  }

  if (o->input) {
    if (pipe(inp) != 0) {
      snprintf(emsg, emsg_sz, "pipe: %s", strerror(errno));
      goto fail;
    }
  } else {
    inp[0] = open("/dev/null", O_RDONLY);
    if (inp[0] < 0) {
      snprintf(emsg, emsg_sz, "open /dev/null: %s", strerror(errno));
      goto fail;
    }
  }
  if (pipe(outp) != 0 || pipe(errp) != 0 || pipe(exep) != 0) {
    snprintf(emsg, emsg_sz, "pipe: %s", strerror(errno));
    goto fail;
  }
  /* Every descriptor closes on exec; dup2 below gives the child plain
   * copies on 0/1/2 only. */
  if (set_cloexec(inp[0]) != 0 || (inp[1] >= 0 && set_cloexec(inp[1]) != 0) ||
      set_cloexec(outp[0]) != 0 || set_cloexec(outp[1]) != 0 ||
      set_cloexec(errp[0]) != 0 || set_cloexec(errp[1]) != 0 ||
      set_cloexec(exep[0]) != 0 || set_cloexec(exep[1]) != 0) {
    snprintf(emsg, emsg_sz, "fcntl: %s", strerror(errno));
    goto fail;
  }

  t0 = astd_run_clock_ms();
  pid = fork();
  if (pid < 0) {
    snprintf(emsg, emsg_sz, "fork: %s", strerror(errno));
    goto fail;
  }
  if (pid == 0) {
    /* Ignored dispositions survive exec: give the CLI the default back. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    (void)sigaction(SIGPIPE, &dfl, NULL);
    if (dup2(inp[0], 0) < 0 || dup2(outp[1], 1) < 0 || dup2(errp[1], 2) < 0)
      child_report(exep[1], 'p');
    if (o->cwd && chdir(o->cwd) != 0) child_report(exep[1], 'c');
    execve(o->argv[0], o->argv, o->envp);
    child_report(exep[1], 'x');
  }
  close_fd(&inp[0]);
  close_fd(&outp[1]);
  close_fd(&errp[1]);
  close_fd(&exep[1]);

  {
    /* exec status: EOF means exec succeeded (CLOEXEC closed the pipe). */
    char m[1 + sizeof(int)];
    size_t used = 0;
    ssize_t got = 0;
    while (used < sizeof m) {
      got = read(exep[0], m + used, sizeof m - used);
      if (got > 0)
        used += (size_t)got;
      else if (got == 0 || errno != EINTR)
        break;
    }
    close_fd(&exep[0]);
    if (used > 0) {
      int ce = 0;
      if (used == sizeof m) memcpy(&ce, m + 1, sizeof ce);
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
      pid = -1;
      res->exec_errno = ce ? ce : EIO;
      if (used == sizeof m && m[0] == 'c')
        snprintf(emsg, emsg_sz, "cannot enter '%s': %s",
                 o->cwd ? o->cwd : "", strerror(ce));
      else
        snprintf(emsg, emsg_sz, "cannot execute '%s': %s", o->argv[0],
                 strerror(res->exec_errno));
      goto fail;
    }
  }

  if (set_nonblock(outp[0]) != 0 || set_nonblock(errp[0]) != 0 ||
      (inp[1] >= 0 && set_nonblock(inp[1]) != 0)) {
    snprintf(emsg, emsg_sz, "fcntl: %s", strerror(errno));
    goto fail_kill;
  }
  if (inp[1] >= 0 && o->input_n == 0) close_fd(&inp[1]);
  if (o->timeout_ms > 0) deadline = t0 + o->timeout_ms;

  for (;;) {
    struct pollfd p[3];
    int slot[3];
    nfds_t np = 0;
    nfds_t k;
    int rc;
    now = astd_run_clock_ms();
    if (!reaped) {
      pid_t got = waitpid(pid, &st, WNOHANG);
      if (got == pid) {
        reaped = 1;
        drain_until = now + DLG_DRAIN_MS;
        close_fd(&inp[1]);
      } else if (got < 0 && errno != EINTR) {
        reaped = 1; /* ECHILD: status lost; report exit -1 */
        no_status = 1;
        drain_until = now + DLG_DRAIN_MS;
      }
    }
    if (!reaped && deadline && !term_sent && now >= deadline) {
      (void)kill(pid, SIGTERM);
      term_sent = 1;
      res->timed_out = 1;
      kill_at = now + (o->grace_ms > 0 ? o->grace_ms : 0);
    }
    if (!reaped && term_sent && !kill_sent && now >= kill_at) {
      (void)kill(pid, SIGKILL);
      kill_sent = 1;
    }
    if (reaped && ((outp[0] < 0 && errp[0] < 0) || now >= drain_until))
      break;

    if (outp[0] >= 0) {
      p[np].fd = outp[0];
      p[np].events = POLLIN;
      p[np].revents = 0;
      slot[np++] = 1;
    }
    if (errp[0] >= 0) {
      p[np].fd = errp[0];
      p[np].events = POLLIN;
      p[np].revents = 0;
      slot[np++] = 2;
    }
    if (inp[1] >= 0) {
      p[np].fd = inp[1];
      p[np].events = POLLOUT;
      p[np].revents = 0;
      slot[np++] = 0;
    }
    rc = poll(p, np, DLG_TICK_MS);
    if (rc < 0) {
      if (errno == EINTR) continue;
      snprintf(emsg, emsg_sz, "poll: %s", strerror(errno));
      goto fail_kill;
    }
    for (k = 0; rc > 0 && k < np; k++) {
      if (p[k].revents == 0) continue;
      if (slot[k] == 1) {
        pump(o, res, &lb, &outp[0], 1);
      } else if (slot[k] == 2) {
        pump(o, res, &lb, &errp[0], 0);
      } else if (inp[1] >= 0) {
        size_t chunk = o->input_n - inoff;
        ssize_t n;
        if (chunk > 65536) chunk = 65536;
        n = write(inp[1], o->input + inoff, chunk);
        if (n > 0) {
          inoff += (size_t)n;
          if (inoff >= o->input_n) close_fd(&inp[1]);
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
          close_fd(&inp[1]); /* EPIPE: the CLI stopped reading */
        }
      }
    }
  }

  /* A last line without its newline is still a line. */
  if (lb.n > 0 && !lb.skipping) dispatch(o, res, &lb);
  close_fd(&inp[1]);
  close_fd(&outp[0]);
  close_fd(&errp[0]);
  free(lb.buf);
  if (no_status)
    res->exit_code = -1;
  else if (WIFEXITED(st))
    res->exit_code = WEXITSTATUS(st);
  else if (WIFSIGNALED(st))
    res->exit_code = -WTERMSIG(st);
  res->duration_ms = astd_run_clock_ms() - t0;
  rc_out = 0;
  goto done;

fail_kill:
  if (pid > 0 && !reaped) {
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) continue;
  }
fail:
  close_fd(&inp[0]);
  close_fd(&inp[1]);
  close_fd(&outp[0]);
  close_fd(&outp[1]);
  close_fd(&errp[0]);
  close_fd(&errp[1]);
  close_fd(&exep[0]);
  close_fd(&exep[1]);
  free(lb.buf);
done:
  (void)sigaction(SIGPIPE, &old_pipe, NULL);
  return rc_out;
}

#endif /* POSIX */
