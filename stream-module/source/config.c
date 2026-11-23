// SPDX-License-Identifier: MIT
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "stream.h"

void stream_ensure_log_dir(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SimpleStreamer", 0777);
    mkdir(STREAM_LOG_DIR, 0777);
}

void stream_config_defaults(StreamConfig* cfg) {
    cfg->streaming_enabled = true;
    cfg->audio_enabled     = true;
    cfg->usb_enabled       = true;
    cfg->video_port        = STREAM_PORT;
    cfg->audio_port        = AUDIO_STREAM_PORT;
}

static bool write_default_config(const char* path, const StreamConfig* def) {
    stream_ensure_log_dir();
    FILE* f = fopen(path, "w");
    if (!f) {
        return false;
    }

    int written = fprintf(
        f,
        "# SimpleStreamer boot configuration\n"
        "# Booleans accept: true, false, 1, 0\n"
        "# Ports accept: 1-65535 (video_port and audio_port must differ)\n"
        "# Delete this file to regenerate defaults.\n"
        "\n"
        "streaming_enabled = %s\n"
        "audio_enabled = %s\n"
        "usb_enabled = %s\n"
        "video_port = %u\n"
        "audio_port = %u\n",
        def->streaming_enabled ? "true" : "false",
        def->audio_enabled     ? "true" : "false",
        def->usb_enabled       ? "true" : "false",
        (unsigned)def->video_port,
        (unsigned)def->audio_port
    );

    if (fclose(f) != 0 || written < 0) {
        return false;
    }
    return true;
}

static char* trim_inplace(char* s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static bool parse_bool_strict(const char* val, bool* out) {
    if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_port_strict(const char* val, u16* out) {
    if (!val || *val == '\0') {
        return false;
    }
    unsigned long port = 0;
    for (const char* p = val; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        port = port * 10UL + (unsigned long)(*p - '0');
        if (port > 65535UL) {
            return false;
        }
    }
    if (port == 0UL) {
        return false;
    }
    *out = (u16)port;
    return true;
}

static void log_cfg_error(int line_num, const char* reason) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "[Config] Malformed %s at line %d (%s); ignoring file and using defaults.",
             STREAM_CFG_PATH, line_num, reason);
    stream_write_log(msg);
}

void stream_config_load(StreamConfig* cfg, bool sd_mounted) {
    stream_config_defaults(cfg);
    if (!sd_mounted) {
        stream_write_log("[Config] SD not mounted; using default settings.");
        return;
    }

    FILE* f = fopen(STREAM_CFG_PATH, "r");
    if (!f) {
        int err = errno;
        if (err == ENOENT) {
            if (write_default_config(STREAM_CFG_PATH, cfg)) {
                stream_write_log("[Config] Config not found; created default " STREAM_CFG_PATH ".");
            } else {
                char msg[200];
                snprintf(msg, sizeof(msg),
                         "[Config] Config not found and failed to create %s (errno=%d); using defaults.",
                         STREAM_CFG_PATH, errno);
                stream_write_log(msg);
            }
        } else {
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "[Config] Failed to open %s (errno=%d); using defaults.",
                     STREAM_CFG_PATH, err);
            stream_write_log(msg);
        }
        return;
    }

    StreamConfig parsed;
    stream_config_defaults(&parsed);

    bool seen_streaming  = false;
    bool seen_audio      = false;
    bool seen_usb        = false;
    bool seen_video_port = false;
    bool seen_audio_port = false;
    int video_port_line  = 0;
    int audio_port_line  = 0;

    char buf[256];
    int line_num = 0;
    bool ok = true;

    while (fgets(buf, sizeof(buf), f) != NULL) {
        line_num++;
        size_t raw_len = strlen(buf);

        // Reject lines that exceed our buffer without terminating at newline or EOF
        if (raw_len == sizeof(buf) - 1 && buf[raw_len - 1] != '\n') {
            int next_ch = fgetc(f);
            if (next_ch != EOF && next_ch != '\n') {
                log_cfg_error(line_num, "line too long");
                ok = false;
                break;
            }
        }

        char* p = buf;

        // Strip UTF-8 BOM on the first line if present (e.g. saved by Windows Notepad)
        if (line_num == 1 &&
            (unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) {
            p += 3;
        }

        // Strip inline '#' comments before trimming
        char* hash = strchr(p, '#');
        if (hash) {
            *hash = '\0';
        }

        p = trim_inplace(p);
        if (*p == '\0') {
            continue;
        }

        char* eq = strchr(p, '=');
        if (!eq) {
            log_cfg_error(line_num, "expected 'key = value' syntax");
            ok = false;
            break;
        }

        // Reject extra '=' characters inside value (not valid for any bool/port setting)
        if (strchr(eq + 1, '=') != NULL) {
            log_cfg_error(line_num, "unexpected extra '='");
            ok = false;
            break;
        }

        *eq = '\0';
        char* key = trim_inplace(p);
        char* val = trim_inplace(eq + 1);

        if (*key == '\0') {
            log_cfg_error(line_num, "empty key");
            ok = false;
            break;
        }
        if (*val == '\0') {
            char reason[96];
            snprintf(reason, sizeof(reason), "empty value for '%s'", key);
            log_cfg_error(line_num, reason);
            ok = false;
            break;
        }

        if (strcmp(key, "streaming_enabled") == 0) {
            if (seen_streaming) {
                log_cfg_error(line_num, "duplicate key 'streaming_enabled'");
                ok = false;
                break;
            }
            if (!parse_bool_strict(val, &parsed.streaming_enabled)) {
                log_cfg_error(line_num, "invalid boolean for 'streaming_enabled'");
                ok = false;
                break;
            }
            seen_streaming = true;
        } else if (strcmp(key, "audio_enabled") == 0) {
            if (seen_audio) {
                log_cfg_error(line_num, "duplicate key 'audio_enabled'");
                ok = false;
                break;
            }
            if (!parse_bool_strict(val, &parsed.audio_enabled)) {
                log_cfg_error(line_num, "invalid boolean for 'audio_enabled'");
                ok = false;
                break;
            }
            seen_audio = true;
        } else if (strcmp(key, "usb_enabled") == 0) {
            if (seen_usb) {
                log_cfg_error(line_num, "duplicate key 'usb_enabled'");
                ok = false;
                break;
            }
            if (!parse_bool_strict(val, &parsed.usb_enabled)) {
                log_cfg_error(line_num, "invalid boolean for 'usb_enabled'");
                ok = false;
                break;
            }
            seen_usb = true;
        } else if (strcmp(key, "video_port") == 0) {
            if (seen_video_port) {
                log_cfg_error(line_num, "duplicate key 'video_port'");
                ok = false;
                break;
            }
            if (!parse_port_strict(val, &parsed.video_port)) {
                log_cfg_error(line_num, "invalid port for 'video_port' (expected 1-65535)");
                ok = false;
                break;
            }
            seen_video_port = true;
            video_port_line = line_num;
        } else if (strcmp(key, "audio_port") == 0) {
            if (seen_audio_port) {
                log_cfg_error(line_num, "duplicate key 'audio_port'");
                ok = false;
                break;
            }
            if (!parse_port_strict(val, &parsed.audio_port)) {
                log_cfg_error(line_num, "invalid port for 'audio_port' (expected 1-65535)");
                ok = false;
                break;
            }
            seen_audio_port = true;
            audio_port_line = line_num;
        } else {
            char reason[96];
            snprintf(reason, sizeof(reason), "unknown key '%.48s'", key);
            log_cfg_error(line_num, reason);
            ok = false;
            break;
        }
    }

    if (ok && ferror(f)) {
        log_cfg_error(line_num > 0 ? line_num : 1, "file read error");
        ok = false;
    }

    fclose(f);

    if (ok && parsed.video_port == parsed.audio_port) {
        int bad_line = (video_port_line > audio_port_line) ? video_port_line : audio_port_line;
        log_cfg_error(bad_line > 0 ? bad_line : 1, "video_port and audio_port cannot be identical");
        ok = false;
    }

    if (!ok) {
        stream_config_defaults(cfg);
        return;
    }

    *cfg = parsed;
    char msg[200];
    snprintf(msg, sizeof(msg),
             "[Config] Loaded %s (streaming=%s, audio=%s, usb=%s, video_port=%u, audio_port=%u)",
             STREAM_CFG_PATH,
             cfg->streaming_enabled ? "true" : "false",
             cfg->audio_enabled     ? "true" : "false",
             cfg->usb_enabled       ? "true" : "false",
             (unsigned)cfg->video_port,
             (unsigned)cfg->audio_port);
    stream_write_log(msg);
}
