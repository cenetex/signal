#include "msg.h"
#include <stdio.h>
#include <string.h>
static int f = 0;
#define T(c,m) do { if (!(c)) { printf("FAIL: %s (got %d)\n", m, __LINE__); f++; } } while(0)
static void mk(uint8_t p[32], uint8_t v) { memset(p, v, 32); }
int main(void) {
    solana_message_t msg;
    uint8_t bh[32]; memset(bh, 0xab, 32);
    solana_message_init(&msg, bh);
    uint8_t fp[32], pg[32], a1[32];
    mk(fp,0x01); mk(pg,0x02); mk(a1,0x03);
    solana_message_add_account(&msg, fp, true, true);
    solana_message_add_account(&msg, pg, false, false);
    solana_message_add_account(&msg, a1, false, true);
    uint8_t d[]={0xde,0xad,0xbe,0xef};
    const uint8_t *ac[]={a1};
    bool s[]={false}, w[]={true};
    solana_message_add_instruction(&msg, pg, ac, s, w, 1, d, 4);
    solana_message_build(&msg);
    printf("header: sig=%d ro_sig=%d ro_un=%d\n", msg.num_required_signatures, msg.num_readonly_signed, msg.num_readonly_unsigned);
    printf("accounts: %d\n", msg.account_count);
    for (int i=0;i<msg.account_count;i++) printf("  %d: sig=%d wr=%d first_byte=%02x\n", i, msg.account_is_signer[i], msg.account_is_writable[i], msg.account_pubkeys[i][0]);
    uint8_t buf[2048];
    int len = solana_message_serialize(&msg, buf, sizeof(buf));
    printf("serialized: %d bytes, header: %d %d %d\n", len, buf[0], buf[1], buf[2]);
    T(buf[0] == 1, "num_sig");
    T(buf[2] == 1, "num_ro_unsigned");
    T(buf[4] == 0x01, "acct0");
    T(buf[36] == 0x03, "acct1");
    T(buf[68] == 0x02, "acct2");
    T(buf[100] == 0xab, "blockhash");
    T(buf[133] == 2, "prog_idx");
    T(len == 141, "len");
    if (f==0) printf("msg: all passed\n");
    else printf("msg: %d failed\n", f);
    return f>0;
}
