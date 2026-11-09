// IC Balance Viewer v1 for New 3DS (Japanese model, built-in FeliCa reader)
// ---------------------------------------------------------------------------
// Read-only viewer for Japanese transit IC cards (Suica / PASMO / ICOCA /
// Kitaca / TOICA / manaca / SUGOCA / nimoca / Hayakaken ...).
// Shows the card balance and the last 20 usage-history entries.
// No payment functionality, no writes to the card: it only uses FeliCa's
// unauthenticated "Read Without Encryption" command, the same access a
// station balance checker uses.
//
// All parameters below were verified on real hardware by the PoC
// (2026-11-09, see ../ic-balance-poc and its icpoc_log.txt):
//   - nfcInit(NFC_OpType_RawNFC), then
//     nfcStartOtherTagScanning(0, 0x04)  -> mask 0x04 selects FeliCa.
//   - nfcGetTagInfo(): id_offset_size == 8, id[0..7] is the FeliCa IDm.
//   - nfcSendTagCommand() wants FeliCa frames WITH the leading length byte.
//   - Balance: service 0x008B block 0, little-endian u16 at bytes 11-12.
//     (Cross-checked: equals the newest history entry's balance, and the
//     other candidate offset gave 64000 yen, above the 20000 yen SF cap.)
//   - History: service 0x090F, 20 blocks. Per-block balance is LE u16 at
//     bytes 10-11, date is packed into bytes 4-5.

#include <3ds.h>
#include <3ds/services/nfc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NFC_TIMEOUT_US (200ULL * 1000000ULL)
#define FELICA_MASK 0x04

static void pump_gfx(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
}

// ---------------------------------------------------------------------------
// FeliCa helpers (frames always carry the leading length byte)
// ---------------------------------------------------------------------------
static int build_rwe(u8 *out, const u8 *idm, u16 service,
                     const u8 *blocks, int nblocks)
{
    int n = 0;
    out[n++] = 0; // length placeholder
    out[n++] = 0x06; // Read Without Encryption
    memcpy(out + n, idm, 8); n += 8;
    out[n++] = 0x01;
    out[n++] = (u8)(service & 0xFF);
    out[n++] = (u8)(service >> 8);
    out[n++] = (u8)nblocks;
    for (int i = 0; i < nblocks; i++) {
        out[n++] = 0x80;
        out[n++] = blocks[i];
    }
    out[0] = (u8)n;
    return n;
}

// Parse an RWE response. Returns block count or negative on failure.
static int parse_rwe(const u8 *resp, int len, const u8 *idm,
                     u8 blocks_out[][16], int maxblocks)
{
    for (int i = 0; i + 8 <= len; i++) {
        if (memcmp(resp + i, idm, 8) != 0) continue;
        int p = i + 8;
        if (len - p < 3) return -1;
        if (resp[p] != 0 || resp[p + 1] != 0) return -2; // status flags
        int nb = resp[p + 2];
        int avail = (len - (p + 3)) / 16;
        int copy = nb;
        if (copy > avail) copy = avail;
        if (copy > maxblocks) copy = maxblocks;
        for (int b = 0; b < copy; b++)
            memcpy(blocks_out[b], resp + p + 3 + b * 16, 16);
        return copy;
    }
    return -1;
}

// Read nblocks of one service starting at first_block. Returns count or <0.
static int read_blocks(const u8 *idm, u16 service, int first_block,
                       int nblocks, u8 blocks_out[][16])
{
    u8 cmd[80];
    static u8 resp[0x800];
    u8 list[16];
    for (int i = 0; i < nblocks; i++) list[i] = (u8)(first_block + i);
    int clen = build_rwe(cmd, idm, service, list, nblocks);
    size_t actual = 0;
    Result ret = nfcSendTagCommand(cmd, clen, resp, sizeof(resp), &actual,
                                   NFC_TIMEOUT_US);
    if (R_FAILED(ret)) return -10;
    return parse_rwe(resp, (int)actual, idm, blocks_out, nblocks);
}

// ---------------------------------------------------------------------------
// History entry decoding (service 0x090F block layout)
// ---------------------------------------------------------------------------
typedef struct {
    int year, mon, day;
    int proc;          // process type code (byte 1)
    int term;          // terminal type (byte 0)
    int entry_line, entry_sta, exit_line, exit_sta;
    int balance;       // balance after this transaction (bytes 10-11 LE)
    int seq;           // sequence number (bytes 13-14 BE + byte 15 region?)
    u8 raw[16];
} HistEntry;

static int block_is_empty(const u8 *b)
{
    for (int i = 0; i < 16; i++) if (b[i] != 0 && b[i] != 0xFF) return 0;
    return 1;
}

static void decode_hist(const u8 *b, HistEntry *e)
{
    memcpy(e->raw, b, 16);
    e->term = b[0];
    e->proc = b[1];
    unsigned date = (b[4] << 8) | b[5];
    e->year = 2000 + ((date >> 9) & 0x7F);
    e->mon  = (date >> 5) & 0x0F;
    e->day  = date & 0x1F;
    e->entry_line = b[6];
    e->entry_sta  = b[7];
    e->exit_line  = b[8];
    e->exit_sta   = b[9];
    e->balance = b[10] | (b[11] << 8);
    e->seq = (b[13] << 8) | b[14];
}

// Process labels (from public Suica format documentation / suica-viewer):
//   0x01 fare (gate exit)   0x02 charge        0x03 ticket purchase
//   0x04 fare adjustment    0x05 entry         0x06 exit
//   0x0D bus                0x0F bus           0x1F bus charge
//   0x46 shopping (e-money)
static const char *proc_label(int proc)
{
    switch (proc) {
        case 0x01: return "TRAIN";
        case 0x02: return "CHARGE";
        case 0x03: return "TICKET";
        case 0x04: return "ADJUST";
        case 0x05: return "ENTER";
        case 0x06: return "EXIT";
        case 0x0D: return "BUS";
        case 0x0F: return "BUS";
        case 0x1F: return "BUS+CHG";
        case 0x46: return "SHOP";
        default:   return NULL;
    }
}

// ---------------------------------------------------------------------------
// Scan control: returns 1 when a tag is InRange, 0 on START (exit request)
// ---------------------------------------------------------------------------
static int wait_for_tag(void)
{
    while (aptMainLoop()) {
        NFC_TagState st;
        if (R_SUCCEEDED(nfcGetTagState(&st)) && st == NFC_TagState_InRange)
            return 1;
        hidScanInput();
        if (hidKeysDown() & KEY_START) return 0;
        svcSleepThread(50000000LL);
        pump_gfx();
    }
    return 0;
}

// Wait until the tag leaves (state OutOfRange) or START is pressed.
static int wait_for_removal(void)
{
    while (aptMainLoop()) {
        NFC_TagState st;
        if (R_SUCCEEDED(nfcGetTagState(&st)) && st == NFC_TagState_OutOfRange)
            return 1;
        hidScanInput();
        if (hidKeysDown() & KEY_START) return 0;
        svcSleepThread(50000000LL);
        pump_gfx();
    }
    return 0;
}

static void print_header(void)
{
    consoleClear();
    printf("IC Balance Viewer v1  (read-only)\n");
    printf("==================================\n");
}

// Read and display one card. Assumes the tag is InRange.
static void show_card(void)
{
    svcSleepThread(200000000LL); // let the tag settle

    NFC_TagInfo ti;
    memset(&ti, 0, sizeof(ti));
    if (R_FAILED(nfcGetTagInfo(&ti))) {
        printf("\nGetTagInfo failed. Keep the card still and retry.\n");
        return;
    }
    const u8 *idm = ti.id; // id_offset_size was 8 on the verified card

    // balance: service 0x008B block 0, LE u16 at bytes 11-12
    static u8 attr[1][16];
    if (read_blocks(idm, 0x008B, 0, 1, attr) < 1) {
        printf("\nBalance read failed.\n");
        printf("Keep the card flat on the lower screen.\n");
        return;
    }
    int balance = attr[0][11] | (attr[0][12] << 8);

    // history: service 0x090F blocks 0..19 in two reads of 10
    static u8 hist_raw[20][16];
    int nhist = 0;
    int n1 = read_blocks(idm, 0x090F, 0, 10, hist_raw);
    if (n1 > 0) {
        nhist = n1;
        int n2 = read_blocks(idm, 0x090F, 10, 10, hist_raw + 10);
        if (n2 > 0) nhist = 10 + n2;
    }

    static HistEntry hist[20];
    int shown = 0;
    for (int i = 0; i < nhist; i++) {
        if (block_is_empty(hist_raw[i])) continue;
        decode_hist(hist_raw[i], &hist[shown]);
        // sanity: a real entry has a plausible date
        if (hist[shown].mon < 1 || hist[shown].mon > 12) continue;
        shown++;
    }

    print_header();
    printf("IDm: %02X%02X%02X%02X%02X%02X%02X%02X\n",
           idm[0], idm[1], idm[2], idm[3], idm[4], idm[5], idm[6], idm[7]);
    printf("\n");
    printf("        BALANCE:  %d yen\n", balance);
    printf("\n");
    printf("---- history (newest first) ----\n");
    for (int i = 0; i < shown; i++) {
        HistEntry *e = &hist[i];
        char labelbuf[16];
        const char *label = proc_label(e->proc);
        if (!label) {
            snprintf(labelbuf, sizeof(labelbuf), "0x%02X", e->proc);
            label = labelbuf;
        }
        // route display: trains show entry>exit, charges show the station,
        // bus/shop records carry no meaningful station codes.
        char route[20] = "--------";
        if (e->proc == 0x01 || e->proc == 0x05 || e->proc == 0x06 ||
            e->proc == 0x04) {
            snprintf(route, sizeof(route), "%02X%02X>%02X%02X",
                     e->entry_line, e->entry_sta, e->exit_line, e->exit_sta);
        } else if (e->proc == 0x02 || e->proc == 0x03) {
            snprintf(route, sizeof(route), "at %02X%02X",
                     e->entry_line, e->entry_sta);
        }
        // amount of this transaction = this balance - next older balance
        char amount[16] = "       ";
        if (i + 1 < shown) {
            int d = e->balance - hist[i + 1].balance;
            snprintf(amount, sizeof(amount), "%+6d", d);
        }
        printf("%04d-%02d-%02d %-7s %-8s %s BAL %6d\n",
               e->year, e->mon, e->day, label, route, amount, e->balance);
    }
    if (shown == 0) printf("(no history entries)\n");
    printf("\nRemove the card to scan again.\n");
    printf("START: exit\n");
    pump_gfx();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    Result ret = nfcInit(NFC_OpType_RawNFC);
    if (R_FAILED(ret)) {
        printf("nfcInit(RawNFC) failed: 0x%08lX\n", (unsigned long)ret);
        printf("Needs a New 3DS with system >= 10.0.0.\n");
        printf("Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            pump_gfx();
            gspWaitForVBlank();
        }
        gfxExit();
        return 0;
    }

    int running = 1;
    while (running) {
        print_header();
        printf("\nPlace your IC card on the LOWER screen\n");
        printf("and keep it still...\n\n");
        printf("START: exit\n");
        pump_gfx();

        ret = nfcStartOtherTagScanning(NFC_STARTSCAN_DEFAULTINPUT, FELICA_MASK);
        if (R_FAILED(ret)) {
            printf("\nStartOtherTagScanning failed: 0x%08lX\n",
                   (unsigned long)ret);
            printf("Press START to exit.\n");
            while (aptMainLoop()) {
                hidScanInput();
                if (hidKeysDown() & KEY_START) break;
                pump_gfx();
                gspWaitForVBlank();
            }
            break;
        }

        if (!wait_for_tag()) { running = 0; }
        else {
            show_card();
            if (!wait_for_removal()) running = 0;
        }
        nfcStopScanning();
        svcSleepThread(300000000LL);
    }

    nfcExit();
    gfxExit();
    return 0;
}
