/* SPDX-License-Identifier: Apache-2.0 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/drivers/linux_profile.h>

static struct linux_profile s_profile;
static bool s_loaded;
static const char *s_file_override;

void linux_profile_set_file(const char *path) {
  s_file_override = path;
  s_loaded = false;
}

static void prv_defaults(struct linux_profile *p) {
  memset(p, 0, sizeof(*p));
  // Galaxy Watch3 (SM-R850, Tizen 5.5).
  strcpy(p->runtime_dir, "/opt/usr/home/owner/share/pebble");
  strcpy(p->data_dir, "/opt/usr/home/owner/share/pebble");
  strcpy(p->keys[0], "*power-keys");
  strcpy(p->keys[1], "gpio_keys");
  strcpy(p->touch[0], "sec_touchscreen");
  strcpy(p->bezel[0], "tizen_detent");
  p->reclaim_delays_ms[0] = 700;
  p->reclaim_delays_ms[1] = 2000;
  p->reclaim_delays = 2;
}

static char *prv_trim(char *s) {
  while (isspace((unsigned char)*s))
    s++;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1]))
    *--end = 0;
  return s;
}

// Splits "a, b, c" into names. An empty value clears the list.
static void prv_names(char (*names)[48], char *value) {
  memset(names, 0, LINUX_PROFILE_MAX_NAMES * 48);
  unsigned n = 0;
  for (char *item = strtok(value, ","); item && n < LINUX_PROFILE_MAX_NAMES;
       item = strtok(NULL, ",")) {
    item = prv_trim(item);
    if (*item && strlen(item) < 48)
      strcpy(names[n++], item);
  }
}

static void prv_delays(struct linux_profile *p, char *value) {
  p->reclaim_delays = 0;
  for (char *item = strtok(value, ","); item && p->reclaim_delays < LINUX_PROFILE_MAX_DELAYS;
       item = strtok(NULL, ",")) {
    char *end;
    unsigned long ms = strtoul(prv_trim(item), &end, 10);
    if (*end || ms > 60000)
      continue;
    p->reclaim_delays_ms[p->reclaim_delays++] = (unsigned)ms;
  }
}

static void prv_set(struct linux_profile *p, const char *key, char *value, unsigned line) {
  if (!strcmp(key, "runtime_dir") && strlen(value) < sizeof(p->runtime_dir))
    strcpy(p->runtime_dir, value);
  else if (!strcmp(key, "data_dir") && strlen(value) < sizeof(p->data_dir))
    strcpy(p->data_dir, value);
  else if (!strcmp(key, "input.keys"))
    prv_names(p->keys, value);
  else if (!strcmp(key, "input.touch"))
    prv_names(p->touch, value);
  else if (!strcmp(key, "input.bezel"))
    prv_names(p->bezel, value);
  else if (!strcmp(key, "foreground_reclaim_ms"))
    prv_delays(p, value);
  else
    fprintf(stderr, "profile: line %u: unknown or invalid key '%s'\n", line, key);
}

static void prv_load_file(struct linux_profile *p, const char *path) {
  FILE *file = fopen(path, "r");
  if (!file) {
    fprintf(stderr, "profile: cannot read %s; using the built-in Galaxy Watch3 profile\n", path);
    return;
  }
  char text[256];
  unsigned line = 0;
  while (fgets(text, sizeof(text), file)) {
    line++;
    char *hash = strchr(text, '#');
    if (hash)
      *hash = 0;
    char *equals = strchr(text, '=');
    if (!equals)
      continue;
    *equals = 0;
    prv_set(p, prv_trim(text), prv_trim(equals + 1), line);
  }
  fclose(file);
}

const struct linux_profile *linux_profile_get(void) {
  if (!s_loaded) {
    prv_defaults(&s_profile);
    const char *path = s_file_override ? s_file_override : getenv("PBL_DEVICE_PROFILE");
    if (path && *path)
      prv_load_file(&s_profile, path);
    s_loaded = true;
  }
  return &s_profile;
}

bool linux_profile_match(const char (*names)[48], const char *device) {
  for (unsigned i = 0; i < LINUX_PROFILE_MAX_NAMES && names[i][0]; i++) {
    if (names[i][0] == '*' ? strstr(device, names[i] + 1) != NULL : !strcmp(device, names[i]))
      return true;
  }
  return false;
}
