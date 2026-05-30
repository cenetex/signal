#include "txn.h"
#include "../../shared/signal_crypto.h"
#include <stdio.h>
#include <string.h>
static int f = 0;
#define T(c,m) do { if (!(c)) { printf("FAIL: %s\n", m); f++; } } while(0)
static void mk(uint8_t p[32], uint8_t v) { memset(p, v, 32); }
int main(void) {
    /* Generate keypair — NaCl returns pub[32] + secret[64].
       First 32 bytes of secret = seed that produces this pubkey. */
    uint8_t pub[32], nacl_secret[64];
    signal_crypto_keypair(pub, nacl_secret);

    /* Build Solana-format keypair: [seed(32)][pub(32)] */
    uint8_t kp[64];
    /* Re-derive from seed to get the clean 32+32 split */
    signal_crypto_keypair_from_seed(nacl_secret, pub, nacl_secret);
    memcpy(kp, nacl_secret, 32);   /* seed = first 32 of nacl_secret */
    memcpy(kp + 32, pub, 32);      /* pubkey */

    printf("pub first byte: %02x\n", pub[0]);

    /* Build message */
    solana_transaction_t txn;
    memset(&txn, 0, sizeof(txn));
    uint8_t bh[32]; memset(bh, 0xcd, 32);
    solana_message_init(&txn.message, bh);

    uint8_t fp[32], pg[32], a1[32];
    memcpy(fp, pub, 32);
    mk(pg, 0x02); mk(a1, 0x03);

    solana_message_add_account(&txn.message, fp, true, true);
    solana_message_add_account(&txn.message, pg, false, false);
    solana_message_add_account(&txn.message, a1, false, true);

    uint8_t d[] = {0x01, 0x02, 0x03};
    const uint8_t *ac[] = {a1};
    bool s[] = {false}, w[] = {true};
    solana_message_add_instruction(&txn.message, pg, ac, s, w, 1, d, 3);
    solana_message_build(&txn.message);

    /* Sign */
    uint8_t keypairs[1][64];
    memcpy(keypairs[0], kp, 64);
    T(solana_transaction_sign(&txn, keypairs, 1) == 0, "sign ok");
    T(txn.signer_count == 1, "signer count");

    /* Serialize */
    uint8_t raw[4096];
    int raw_len = solana_transaction_serialize(&txn, raw, sizeof(raw));
    T(raw_len > 0, "serialize ok");
    printf("raw tx: %d bytes\n", raw_len);
    T(raw[0] == 1, "sig count byte");

    /* Base64 */
    char b64[8192];
    int b64_len = solana_transaction_to_base64(&txn, b64, sizeof(b64));
    T(b64_len > 0, "base64 encode");
    printf("base64: %.40s... (%d chars)\n", b64, b64_len);

    /* Verify signature */
    uint8_t msg_bytes[2048];
    int msg_len = solana_message_serialize(&txn.message, msg_bytes, sizeof(msg_bytes));
    bool ok = signal_crypto_verify(txn.signatures[0], msg_bytes, msg_len, pub);
    T(ok, "crypto verify");

    if (f == 0) printf("txn: all passed\n");
    else printf("txn: %d failed\n", f);
    return f > 0;
}
