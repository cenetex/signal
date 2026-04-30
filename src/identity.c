/*
 * identity.c -- See identity.h.
 *
 * On-disk format: 64 raw bytes — exactly the secret[64] (seed||pub).
 * The pubkey is recoverable as the trailing 32 bytes; we don't write
 * a separate header so a corrupt file is detectable purely by length.
 */
#include "identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>
#elif defined(_WIN32)
  #include <windows.h>
  #include <shlobj.h>
  #include <direct.h>
  #include <io.h>
  #define mkdir_compat(p) _mkdir(p)
#else
  #include <fcntl.h>
  #include <pwd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define mkdir_compat(p) mkdir((p), 0700)
#endif

/* ---------------------------------------------------------------------
 * Platform path resolution
 * ------------------------------------------------------------------ */

#if !defined(__EMSCRIPTEN__)
static bool resolve_default_path(char *out, size_t cap) {
#if defined(_WIN32)
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        return false;
    }
    int n = snprintf(out, cap, "%s\\signal", base);
    if (n <= 0 || (size_t)n >= cap) return false;
    _mkdir(out);
    char file[MAX_PATH];
    n = snprintf(file, sizeof(file), "%s\\identity.key", out);
    if (n <= 0 || (size_t)n >= sizeof(file)) return false;
    if ((size_t)n >= cap) return false;
    memcpy(out, file, (size_t)n + 1);
    return true;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home) return false;
    char dir[1024];
    int n = snprintf(dir, sizeof(dir),
                     "%s/Library/Application Support/signal", home);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return false;
    /* mkdir each component we control. Library/Application Support
     * already exists on every macOS install. */
    mkdir(dir, 0700);
    n = snprintf(out, cap, "%s/identity.key", dir);
    return n > 0 && (size_t)n < cap;
#else
    /* XDG. */
    const char *xdg = getenv("XDG_DATA_HOME");
    char fallback[1024];
    if (!xdg || !*xdg) {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : NULL;
        }
        if (!home) return false;
        int n = snprintf(fallback, sizeof(fallback),
                         "%s/.local/share", home);
        if (n <= 0 || (size_t)n >= sizeof(fallback)) return false;
        xdg = fallback;
    }
    char dir[1280];
    int n = snprintf(dir, sizeof(dir), "%s/signal", xdg);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return false;
    /* Best-effort mkdir parents; XDG_DATA_HOME may not exist. */
    char parent[1280];
    snprintf(parent, sizeof(parent), "%s", xdg);
    mkdir(parent, 0700);
    mkdir(dir, 0700);
    n = snprintf(out, cap, "%s/identity.key", dir);
    return n > 0 && (size_t)n < cap;
#endif
}
#endif /* !__EMSCRIPTEN__ */

/* ---------------------------------------------------------------------
 * File I/O — POSIX/Windows path
 * ------------------------------------------------------------------ */

#if !defined(__EMSCRIPTEN__)
static bool read_secret_file(const char *path,
                             uint8_t out[SIGNAL_CRYPTO_SECRET_BYTES],
                             bool *out_corrupt) {
    *out_corrupt = false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false; /* missing — fresh-generate path */
    uint8_t buf[SIGNAL_CRYPTO_SECRET_BYTES + 1];
    size_t got = fread(buf, 1, sizeof(buf), fp);
    int eof = feof(fp);
    fclose(fp);
    if (got != SIGNAL_CRYPTO_SECRET_BYTES || !eof) {
        *out_corrupt = true;
        return false;
    }
    memcpy(out, buf, SIGNAL_CRYPTO_SECRET_BYTES);
    return true;
}

static bool write_secret_file(const char *path,
                              const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
#if defined(_WIN32)
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t wrote = fwrite(secret, 1, SIGNAL_CRYPTO_SECRET_BYTES, fp);
    fclose(fp);
    return wrote == SIGNAL_CRYPTO_SECRET_BYTES;
#else
    /* O_CREAT|O_TRUNC|O_WRONLY at 0600 so we never get a world-readable
     * key file even with a permissive umask. */
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return false;
    /* Belt-and-suspenders: chmod in case the file already existed with
     * looser perms and open() didn't reset them. */
    fchmod(fd, 0600);
    size_t off = 0;
    while (off < SIGNAL_CRYPTO_SECRET_BYTES) {
        ssize_t n = write(fd, secret + off,
                          SIGNAL_CRYPTO_SECRET_BYTES - off);
        if (n <= 0) { close(fd); return false; }
        off += (size_t)n;
    }
    close(fd);
    return true;
#endif
}

static void rename_to_bad(const char *path) {
    char bad[2048];
    int n = snprintf(bad, sizeof(bad), "%s.bad", path);
    if (n <= 0 || (size_t)n >= sizeof(bad)) return;
    /* rename overwrites .bad if it already exists on POSIX; that's fine,
     * we only keep the most recent bad copy. */
    remove(bad);
    rename(path, bad);
}
#endif /* !__EMSCRIPTEN__ */

/* ---------------------------------------------------------------------
 * Wasm path: localStorage["signal:identity"] base64 of secret bytes
 * ------------------------------------------------------------------ */

#if defined(__EMSCRIPTEN__)
/* Stash up to 96 base64 chars (88 needed for 64 raw bytes + null pad). */
EM_JS(int, signal_localstorage_load, (char *out, int cap), {
    try {
        var s = window.localStorage.getItem("signal:identity");
        if (!s) return 0;
        if (s.length + 1 > cap) return -1;
        stringToUTF8(s, out, cap);
        return s.length;
    } catch (e) { return -1; }
})

EM_JS(int, signal_localstorage_save, (const char *s), {
    try {
        window.localStorage.setItem("signal:identity", UTF8ToString(s));
        return 1;
    } catch (e) { return 0; }
})

EM_JS(void, signal_localstorage_rename_bad, (), {
    try {
        var s = window.localStorage.getItem("signal:identity");
        if (s) {
            window.localStorage.setItem("signal:identity.bad", s);
            window.localStorage.removeItem("signal:identity");
        }
    } catch (e) {}
})

static const char B64ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, size_t len, char *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = B64ALPHA[(v >> 18) & 63];
        out[o++] = B64ALPHA[(v >> 12) & 63];
        out[o++] = B64ALPHA[(v >> 6)  & 63];
        out[o++] = B64ALPHA[ v        & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i+1] << 8;
        out[o++] = B64ALPHA[(v >> 18) & 63];
        out[o++] = B64ALPHA[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64ALPHA[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool b64_decode(const char *s, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t i = 0, o = 0;
    while (s[i] && s[i] != '=') {
        int v0 = b64_decode_char(s[i]);
        int v1 = (s[i+1] && s[i+1] != '=') ? b64_decode_char(s[i+1]) : -1;
        int v2 = (s[i+1] && s[i+2] && s[i+2] != '=') ? b64_decode_char(s[i+2]) : -1;
        int v3 = (s[i+1] && s[i+2] && s[i+3] && s[i+3] != '=') ? b64_decode_char(s[i+3]) : -1;
        if (v0 < 0 || v1 < 0) return false;
        if (o >= out_cap) return false;
        out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0) {
            if (o >= out_cap) return false;
            out[o++] = (uint8_t)(((v1 & 0xf) << 4) | (v2 >> 2));
        }
        if (v3 >= 0) {
            if (o >= out_cap) return false;
            out[o++] = (uint8_t)(((v2 & 0x3) << 6) | v3);
        }
        i += 4;
        if (s[i] == '\0') break;
    }
    *out_len = o;
    return true;
}
#endif /* __EMSCRIPTEN__ */

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

bool identity_save_to(const player_identity_t *id, const char *path) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    char enc[128];
    b64_encode(id->secret, SIGNAL_CRYPTO_SECRET_BYTES, enc);
    return signal_localstorage_save(enc) == 1;
#else
    return write_secret_file(path, id->secret);
#endif
}

bool identity_load_or_generate_at(player_identity_t *out, const char *path) {
    memset(out, 0, sizeof(*out));

#if defined(__EMSCRIPTEN__)
    (void)path;
    char enc[256];
    int got = signal_localstorage_load(enc, sizeof(enc));
    if (got > 0) {
        size_t dec_len = 0;
        uint8_t dec[SIGNAL_CRYPTO_SECRET_BYTES + 4];
        if (b64_decode(enc, dec, sizeof(dec), &dec_len) &&
            dec_len == SIGNAL_CRYPTO_SECRET_BYTES) {
            memcpy(out->secret, dec, SIGNAL_CRYPTO_SECRET_BYTES);
            memcpy(out->pubkey,
                   out->secret + (SIGNAL_CRYPTO_SECRET_BYTES -
                                  SIGNAL_CRYPTO_PUBKEY_BYTES),
                   SIGNAL_CRYPTO_PUBKEY_BYTES);
            return true;
        }
        /* Corrupt — preserve as .bad before regenerating. */
        fprintf(stderr,
                "[identity] localStorage entry corrupt; "
                "moving to signal:identity.bad\n");
        signal_localstorage_rename_bad();
    }
    signal_crypto_keypair(out->pubkey, out->secret);
    return identity_save_to(out, path);
#else
    bool corrupt = false;
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    if (read_secret_file(path, secret, &corrupt)) {
        memcpy(out->secret, secret, SIGNAL_CRYPTO_SECRET_BYTES);
        memcpy(out->pubkey,
               out->secret + (SIGNAL_CRYPTO_SECRET_BYTES -
                              SIGNAL_CRYPTO_PUBKEY_BYTES),
               SIGNAL_CRYPTO_PUBKEY_BYTES);
        return true;
    }
    if (corrupt) {
        fprintf(stderr,
                "[identity] %s is not 64 bytes; moving to %s.bad\n",
                path, path);
        rename_to_bad(path);
    }
    signal_crypto_keypair(out->pubkey, out->secret);
    return write_secret_file(path, out->secret);
#endif
}

bool identity_load_or_generate(player_identity_t *out) {
#if defined(__EMSCRIPTEN__)
    return identity_load_or_generate_at(out, NULL);
#else
    char path[2048];
    if (!resolve_default_path(path, sizeof(path))) {
        /* Couldn't find a writable path — generate ephemerally so the
         * client at least has a valid keypair this session. */
        fprintf(stderr,
                "[identity] could not resolve a writable identity path; "
                "using ephemeral keypair for this session\n");
        memset(out, 0, sizeof(*out));
        signal_crypto_keypair(out->pubkey, out->secret);
        return true;
    }
    return identity_load_or_generate_at(out, path);
#endif
}
