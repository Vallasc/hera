#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

/* Written by several threads (AT thread, D-Bus thread, reconnect
 * supervisor) and read by the supervisor: write a temporary file and rename
 * it, so that a reader sees the old or the new content, never a truncated
 * one. */
void store_save_last_device(const char *address)
{
    if (!address || !*address)
        return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", g_config.last_device_file,
             (long)syscall(SYS_gettid));
    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_warn("store", "Cannot save last device");
        return;
    }
    int ok = fprintf(f, "%s\n", address) > 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, g_config.last_device_file) != 0) {
        log_warn("store", "Cannot save last device");
        unlink(tmp);
    }
}

char *store_load_last_device(void)
{
    FILE *f = fopen(g_config.last_device_file, "r");
    if (!f)
        return NULL;
    char buf[64] = {0};
    char *r = fgets(buf, sizeof(buf), f);
    fclose(f);
    if (!r)
        return NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] ? strdup(buf) : NULL;
}

char *store_address_from_path(const char *path)
{
    if (!path)
        return NULL;
    const char *slash = strrchr(path, '/');
    const char *tail = slash ? slash + 1 : path;
    if (strncmp(tail, "dev_", 4) != 0)
        return NULL;
    char *out = strdup(tail + 4);
    if (!out)
        return NULL;
    for (char *p = out; *p; p++)
        if (*p == '_')
            *p = ':';
    return out;
}

char *store_path_tail_from_address(const char *address)
{
    if (!address)
        return NULL;
    char *out = malloc(strlen(address) + 5);
    if (!out)
        return NULL;
    strcpy(out, "dev_");
    char *p = out + 4;
    for (const char *s = address; *s; s++)
        *p++ = (*s == ':') ? '_' : *s;
    *p = '\0';
    return out;
}

int store_is_safe_token(const char *s)
{
    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == ':' || c == '_' ||
              c == '-' || c == '.'))
            return 0;
    }
    return 1;
}
