/*
 * signal_wallet_link -- make a RATi link from a Signal identity to a wallet.
 *
 *   signal_wallet_link --identity=<identity.key> --wallet=<base58>
 *                      [--sequence=<n>]
 *                      [--wallet-keypair=<keypair.json> | --wallet-signature=<hex>]
 *
 * Builds the RATi link message (shared/wallet_link.h, Forge
 * docs/rati-link.md), signs it with the 64-byte identity secret the game
 * client saves, and prints the link as JSON. A link is complete once the
 * wallet has signed the same message too:
 *
 *   - --wallet-keypair signs with a Solana CLI keypair file (a JSON array of
 *     64 numbers), or
 *   - run once without it, sign the printed "message" in the wallet (for
 *     example Phantom's signMessage), then run again with the same
 *     --sequence and --wallet-signature=<hex>.
 *
 * Forge's LinkWallet takes a complete link. Secrets are read, used once and
 * cleared; they are never printed.
 *
 * The client saves the identity at:
 *   macOS    ~/Library/Application Support/signal/identity.key
 *   Linux    $XDG_DATA_HOME/signal/identity.key (~/.local/share/signal/...)
 *   Windows  %LOCALAPPDATA%\signal\identity.key
 *
 * Exit: 0 link printed, 1 signing or verification failure, 2 usage or input
 * error.
 */
#include "base58.h"
#include "signal_crypto.h"
#include "signal_memzero.h"
#include "wallet_link.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: signal_wallet_link --identity=<identity.key> --wallet=<base58>\n"
        "                          [--sequence=<n>]\n"
        "                          [--wallet-keypair=<keypair.json> | --wallet-signature=<hex>]\n"
        "\n"
        "Signs a RATi link message with the Signal identity key and prints it as\n"
        "JSON. The link is complete once the wallet signs the same message:\n"
        "pass a Solana keypair file, or sign the printed message in the wallet\n"
        "and rerun with the same --sequence and --wallet-signature.\n"
        "--sequence defaults to the current Unix time; a higher one replaces an\n"
        "older link.\n"
        "\n"
        "Exit: 0 ok, 1 signing or verification failure, 2 input error.\n");
}

static void print_hex(const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) printf("%02x", bytes[i]);
}

/* Print `bytes` as a JSON string body. The message is ASCII with "\n". */
static void print_json_text(const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == '\n') fputs("\\n", stdout);
        else if (bytes[i] == '"' || bytes[i] == '\\') printf("\\%c", bytes[i]);
        else putchar(bytes[i]);
    }
}

static bool read_identity(const char *path,
                          uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t extra = 0;
    bool ok = fread(secret, 1, SIGNAL_CRYPTO_SECRET_BYTES, f) ==
                  SIGNAL_CRYPTO_SECRET_BYTES &&
              fread(&extra, 1, 1, f) == 0;
    fclose(f);
    if (!ok) signal_memzero_explicit(secret, SIGNAL_CRYPTO_SECRET_BYTES);
    return ok;
}

/* A Solana CLI keypair file: a JSON array of exactly 64 numbers 0-255,
 * the secret seed followed by the public key. */
static bool read_solana_keypair(const char *path,
                                uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char text[1024];
    size_t len = fread(text, 1, sizeof(text) - 1, f);
    bool too_long = !feof(f);
    fclose(f);
    text[len] = '\0';
    bool ok = !too_long;
    size_t count = 0;
    const char *p = text;
    while (ok && *p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (ok && *p++ != '[') ok = false;
    while (ok) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p < '0' || *p > '9' || count >= SIGNAL_CRYPTO_SECRET_BYTES) {
            ok = false;
            break;
        }
        unsigned value = 0;
        while (*p >= '0' && *p <= '9' && value <= 255) value = value * 10u + (unsigned)(*p++ - '0');
        if (value > 255) {
            ok = false;
            break;
        }
        secret[count++] = (uint8_t)value;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            p++;
            break;
        }
        ok = false;
    }
    while (ok && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    ok = ok && *p == '\0' && count == SIGNAL_CRYPTO_SECRET_BYTES;
    signal_memzero_explicit(text, sizeof(text));
    if (!ok) signal_memzero_explicit(secret, SIGNAL_CRYPTO_SECRET_BYTES);
    return ok;
}

static bool parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s || *s == '-' || *s == '+') return false;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_sig_hex(const char *s, uint8_t out[SIGNAL_CRYPTO_SIG_BYTES]) {
    if (!s || strlen(s) != 2u * SIGNAL_CRYPTO_SIG_BYTES) return false;
    for (size_t i = 0; i < SIGNAL_CRYPTO_SIG_BYTES; i++) {
        unsigned value = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + (size_t)k];
            unsigned nibble;
            if (c >= '0' && c <= '9') nibble = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = (unsigned)(c - 'A' + 10);
            else return false;
            value = value * 16u + nibble;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

int main(int argc, char **argv) {
    const char *identity_path = NULL;
    const char *wallet_b58 = NULL;
    const char *sequence_text = NULL;
    const char *keypair_path = NULL;
    const char *wallet_sig_hex = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strncmp(argv[i], "--identity=", 11) == 0) {
            identity_path = argv[i] + 11;
        } else if (strncmp(argv[i], "--wallet=", 9) == 0) {
            wallet_b58 = argv[i] + 9;
        } else if (strncmp(argv[i], "--sequence=", 11) == 0) {
            sequence_text = argv[i] + 11;
        } else if (strncmp(argv[i], "--wallet-keypair=", 17) == 0) {
            keypair_path = argv[i] + 17;
        } else if (strncmp(argv[i], "--wallet-signature=", 19) == 0) {
            wallet_sig_hex = argv[i] + 19;
        } else {
            print_usage(stderr);
            return 2;
        }
    }
    uint64_t sequence = (uint64_t)time(NULL);
    if (!identity_path || !wallet_b58 || (keypair_path && wallet_sig_hex) ||
        (sequence_text && !parse_u64(sequence_text, &sequence))) {
        print_usage(stderr);
        return 2;
    }
    uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES];
    if (base58_decode(wallet_b58, wallet, sizeof(wallet)) != sizeof(wallet)) {
        fprintf(stderr, "signal_wallet_link: --wallet is not a 32-byte base58 key\n");
        return 2;
    }
    uint8_t wallet_sig[SIGNAL_CRYPTO_SIG_BYTES] = {0};
    bool have_wallet_sig = false;
    if (wallet_sig_hex) {
        if (!parse_sig_hex(wallet_sig_hex, wallet_sig)) {
            fprintf(stderr, "signal_wallet_link: --wallet-signature is not 64 bytes of hex\n");
            return 2;
        }
        have_wallet_sig = true;
    }

    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    if (!read_identity(identity_path, secret)) {
        fprintf(stderr, "signal_wallet_link: %s is not a 64-byte identity key\n",
                identity_path);
        return 2;
    }
    uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES];
    memcpy(identity, secret + 32, sizeof(identity));
    uint8_t identity_sig[SIGNAL_CRYPTO_SIG_BYTES];
    bool signed_ok = wallet_link_sign(identity_sig, identity, secret, identity,
                                      wallet, sequence);
    signal_memzero_explicit(secret, sizeof(secret));
    if (!signed_ok) {
        fprintf(stderr, "signal_wallet_link: could not sign the link\n");
        return 1;
    }

    if (keypair_path) {
        uint8_t wallet_secret[SIGNAL_CRYPTO_SECRET_BYTES];
        if (!read_solana_keypair(keypair_path, wallet_secret)) {
            fprintf(stderr, "signal_wallet_link: %s is not a Solana keypair file\n",
                    keypair_path);
            return 2;
        }
        bool ok = wallet_link_sign(wallet_sig, wallet, wallet_secret, identity,
                                   wallet, sequence);
        signal_memzero_explicit(wallet_secret, sizeof(wallet_secret));
        if (!ok) {
            fprintf(stderr, "signal_wallet_link: %s is not the keypair of --wallet\n",
                    keypair_path);
            return 2;
        }
        have_wallet_sig = true;
    }
    if (have_wallet_sig &&
        !wallet_link_verify(identity, wallet, sequence, identity_sig, wallet_sig)) {
        fprintf(stderr, "signal_wallet_link: the wallet signature does not match this link"
                        " (same --sequence?)\n");
        return 1;
    }

    uint8_t msg[WALLET_LINK_MESSAGE_MAX];
    size_t msg_len = wallet_link_message(msg, identity, wallet, sequence);
    char identity_b58[64], wallet_out[64];
    base58_encode(identity, sizeof(identity), identity_b58, sizeof(identity_b58));
    base58_encode(wallet, sizeof(wallet), wallet_out, sizeof(wallet_out));
    printf("{\"schema\":\"rati_link_v1\",\"app\":\"" WALLET_LINK_APP "\","
           "\"identity\":\"%s\",\"wallet\":\"%s\",\"sequence\":%llu,\"message\":\"",
           identity_b58, wallet_out, (unsigned long long)sequence);
    print_json_text(msg, msg_len);
    printf("\",\"identity_signature\":\"");
    print_hex(identity_sig, sizeof(identity_sig));
    printf("\",\"wallet_signature\":");
    if (have_wallet_sig) {
        printf("\"");
        print_hex(wallet_sig, sizeof(wallet_sig));
        printf("\"");
    } else {
        printf("null");
    }
    printf(",\"complete\":%s}\n", have_wallet_sig ? "true" : "false");
    return 0;
}
