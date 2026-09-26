/*
 * signal_wallet_link -- sign a link from a Signal identity to a Solana wallet.
 *
 *   signal_wallet_link --identity=<identity.key> --wallet=<base58>
 *                      [--sequence=<n>]
 *
 * Reads the 64-byte identity secret the game client saves, signs a
 * signal_wallet_link_v2 message (shared/wallet_link.h) naming the wallet,
 * and prints the link as JSON. Forge checks this signature on chain before
 * it mints play supply earned by the identity to the wallet. The secret is
 * read, used once and cleared; it is never printed.
 *
 * The client saves the identity at:
 *   macOS    ~/Library/Application Support/signal/identity.key
 *   Linux    $XDG_DATA_HOME/signal/identity.key (~/.local/share/signal/...)
 *   Windows  %LOCALAPPDATA%\signal\identity.key
 *
 * Exit: 0 link printed, 1 signing failure, 2 usage or input error.
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

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: signal_wallet_link --identity=<identity.key> --wallet=<base58>"
        " [--sequence=<n>]\n"
        "\n"
        "Signs a signal_wallet_link_v2 message with the Signal identity key and\n"
        "prints the link as JSON. A higher --sequence (default 1) replaces an\n"
        "older link.\n"
        "\n"
        "Exit: 0 ok, 1 signing failure, 2 input error.\n");
}

static void print_hex(const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) printf("%02x", bytes[i]);
}

static bool read_secret(const char *path,
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

static bool parse_u64(const char *s, uint64_t *out) {
    if (!s || !*s || *s == '-') return false;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

int main(int argc, char **argv) {
    const char *identity_path = NULL;
    const char *wallet_b58 = NULL;
    const char *sequence_text = "1";
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
        } else {
            print_usage(stderr);
            return 2;
        }
    }
    uint64_t sequence = 0;
    if (!identity_path || !wallet_b58 || !parse_u64(sequence_text, &sequence)) {
        print_usage(stderr);
        return 2;
    }
    uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES];
    if (base58_decode(wallet_b58, wallet, sizeof(wallet)) != sizeof(wallet)) {
        fprintf(stderr, "signal_wallet_link: --wallet is not a 32-byte base58 key\n");
        return 2;
    }
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    if (!read_secret(identity_path, secret)) {
        fprintf(stderr, "signal_wallet_link: %s is not a 64-byte identity key\n",
                identity_path);
        return 2;
    }
    uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES];
    memcpy(identity, secret + 32, sizeof(identity));
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    bool signed_ok = wallet_link_sign(sig, identity, secret, wallet, sequence);
    signal_memzero_explicit(secret, sizeof(secret));
    if (!signed_ok || !wallet_link_verify(identity, wallet, sequence, sig)) {
        fprintf(stderr, "signal_wallet_link: could not sign the link\n");
        return 1;
    }

    char identity_b58[64], wallet_out[64];
    base58_encode(identity, sizeof(identity), identity_b58, sizeof(identity_b58));
    base58_encode(wallet, sizeof(wallet), wallet_out, sizeof(wallet_out));
    printf("{\"schema\":\"signal_wallet_link_v2\",\"signal_pubkey\":\"%s\","
           "\"solana_pubkey\":\"%s\",\"sequence\":%llu,\"signature\":\"",
           identity_b58, wallet_out, (unsigned long long)sequence);
    print_hex(sig, sizeof(sig));
    printf("\"}\n");
    return 0;
}
