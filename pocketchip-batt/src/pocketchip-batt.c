/*
 * Publish PocketCHIP battery voltage and charging state for pocket-home and
 * the pocketchip-warn/off timers, from the axp20x power_supply class.
 *
 *   voltage   integer mV, running average avg = (sample + avg) / 2
 *   charging  "1" while the battery status is "Charging", else "0"
 *
 * Files are replaced atomically and only when their content changes. The
 * battery is sampled every period; a power_supply uevent (charger plugged or
 * unplugged) resamples the charging state immediately.
 *
 * Environment: POCKETCHIP_BATT_SYSFS (/sys/class/power_supply),
 * POCKETCHIP_BATT_DIR (/run/pocketchip-batt), POCKETCHIP_BATT_PERIOD_MS.
 * Argument --once: sample once, write both files and exit.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BATTERY "axp20x-battery"
#define FALLBACK_MV 3700L
#define DEFAULT_PERIOD_MS 10000L

struct output {
  const char *name;
  char last[24];
};

static const char *sysfs, *outdir;
static volatile sig_atomic_t stop;

static void on_signal(int sig) { stop = sig; }

static const char *env_or(const char *name, const char *def) {
  const char *v = getenv(name);
  return v && *v ? v : def;
}

static int read_attr(const char *attr, char *buf, size_t len) {
  char path[PATH_MAX];
  int fd;
  ssize_t n;

  snprintf(path, sizeof path, "%s/" BATTERY "/%s", sysfs, attr);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  n = read(fd, buf, len - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = '\0';
  buf[strcspn(buf, "\n")] = '\0';
  return 0;
}

static int sample_mv(long *mv) {
  char buf[32], *end;
  long uv;

  if (read_attr("voltage_now", buf, sizeof buf))
    return -1;
  errno = 0;
  uv = strtol(buf, &end, 10);
  if (end == buf || *end || errno || uv <= 0)
    return -1;
  *mv = uv / 1000;
  return 0;
}

static int sample_charging(void) {
  char buf[32];
  return !read_attr("status", buf, sizeof buf) && !strcmp(buf, "Charging");
}

static void publish(struct output *o, const char *val) {
  char dst[PATH_MAX], tmp[PATH_MAX];
  size_t len = strlen(val);
  int fd, ok;

  if (!strcmp(o->last, val))
    return;
  snprintf(dst, sizeof dst, "%s/%s", outdir, o->name);
  snprintf(tmp, sizeof tmp, "%s/.%s.tmp", outdir, o->name);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    perror(tmp);
    return;
  }
  ok = write(fd, val, len) == (ssize_t)len;
  ok = !close(fd) && ok;
  if (ok && !rename(tmp, dst)) {
    snprintf(o->last, sizeof o->last, "%s", val);
    return;
  }
  perror(dst);
  unlink(tmp);
}

static void update_voltage(struct output *o, long *avg) {
  char buf[24];
  long mv;

  if (!sample_mv(&mv))
    *avg = *avg < 0 ? mv : (mv + *avg) / 2;
  else if (*avg < 0)
    *avg = FALLBACK_MV;
  snprintf(buf, sizeof buf, "%ld\n", *avg);
  publish(o, buf);
}

static void update_charging(struct output *o) {
  publish(o, sample_charging() ? "1\n" : "0\n");
}

static int uevent_open(void) {
  struct sockaddr_nl sa = {.nl_family = AF_NETLINK, .nl_groups = 1};
  int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                  NETLINK_KOBJECT_UEVENT);

  if (fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof sa)) {
    close(fd);
    fd = -1;
  }
  return fd;
}

static int uevent_power_supply(int fd) {
  char buf[8192];
  ssize_t n;
  int hit = 0;

  while ((n = recv(fd, buf, sizeof buf - 1, 0)) > 0) {
    buf[n] = '\0';
    for (char *p = buf; p < buf + n; p += strlen(p) + 1)
      hit |= !strcmp(p, "SUBSYSTEM=power_supply");
  }
  return hit;
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv) {
  struct output voltage = {"voltage", ""}, charging = {"charging", ""};
  long avg = -1, period, next;
  int once = argc > 1 && !strcmp(argv[1], "--once");
  struct pollfd pfd = {.events = POLLIN};
  struct sigaction sa = {.sa_handler = on_signal};

  sysfs = env_or("POCKETCHIP_BATT_SYSFS", "/sys/class/power_supply");
  outdir = env_or("POCKETCHIP_BATT_DIR", "/run/pocketchip-batt");
  period = strtol(env_or("POCKETCHIP_BATT_PERIOD_MS", ""), NULL, 10);
  if (period <= 0)
    period = DEFAULT_PERIOD_MS;
  if (argc > 1 && !once) {
    fprintf(stderr, "usage: %s [--once]\n", argv[0]);
    return 2;
  }

  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  pfd.fd = once ? -1 : uevent_open();
  for (next = now_ms(); !stop;) {
    long wait = next - now_ms();
    int r;

    if (wait <= 0) {
      update_voltage(&voltage, &avg);
      update_charging(&charging);
      if (once)
        return 0;
      next = now_ms() + period;
      continue;
    }
    r = poll(&pfd, 1, (int)wait);
    if (r > 0 && uevent_power_supply(pfd.fd))
      update_charging(&charging);
    else if (r < 0 && errno != EINTR)
      return 1;
  }
  return 0;
}
