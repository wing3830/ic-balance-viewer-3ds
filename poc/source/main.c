// IC Balance PoC for New 3DS (Japanese model, built-in FeliCa reader)
// ------------------------------------------------------------------
// READ-ONLY proof of concept. It never writes to the card and has no
// payment functionality whatsoever. It only uses the FeliCa commands
// that do not require authentication:
//   - Request Service        (0x02)
//   - Request Response       (0x04)
//   - Read Without Encryption(0x06)  -> service 0x008B (balance)
//                                       service 0x090F (history)
// This is the same class of access as a station balance checker or an
// Android Suica reader app.
//
// Unknowns this PoC is designed to settle on real hardware:
//   1. Which StartOtherTagScanning bitmask (unk1) selects FeliCa tags.
//      (Thenaya uses 0x01 for Type-A NTAG/amiibo. The FeliCa bit is not
//      documented, so we sweep all values and log which one hits.)
//   2. Whether nfcGetTagInfo() exposes the FeliCa IDm, and where.
//   3. Whether nfcSendTagCommand() wants FeliCa frames WITH or WITHOUT
//      the leading length byte. Both variants are tried and logged.
// Everything is written to sdmc:/icpoc_log.txt as well as the screen.

#include <3ds.h>
#include <3ds/services/nfc.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#define NFC_TIMEOUT_US (200ULL * 1000000ULL) // same value Thenaya uses

static FILE *g_log = NULL;
static int g_user_cancel = 0;

// ------------------------------------------------------------------
// logging helpers
// ------------------------------------------------------------------
static void pump_gfx(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
}

static void logprintf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    if (g_log) {
        fputs(buf, g_log);
        fflush(g_log);
    }
    pump_gfx();
}

static void hexdump(const char *label, const u8 *data, int len)
{
    logprintf("%s (%d bytes):\n", label, len);
    for (int off = 0; off < len; off += 16) {
        char line[128];
        int n = snprintf(line, sizeof(line), "  %04X:", off);
        int row = len - off;
        if (row > 16) row = 16;
        for (int i = 0; i < row; i++)
            n += snprintf(line + n, sizeof(line) - n, " %02X", data[off + i]);
        strncat(line, "\n", sizeof(line) - strlen(line) - 1);
        logprintf("%s", line);
    }
}

// returns 1 if the user pressed B (cancel everything) while waiting
static int wait_ms(int ms)
{
    int steps = ms / 50;
    for (int i = 0; i < steps; i++) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_B) { g_user_cancel = 1; return 1; }
        if (k & KEY_START) { g_user_cancel = 2; return 1; }
        svcSleepThread(50000000LL); // 50ms
        pump_gfx();
    }
    return 0;
}

// ------------------------------------------------------------------
// FeliCa frame builders. with_len=1 prefixes the FeliCa length byte.
// ------------------------------------------------------------------
static int build_frame(u8 *out, const u8 *payload, int payload_len, int with_len)
{
    if (with_len) {
        out[0] = (u8)(payload_len + 1);
        memcpy(out + 1, payload, payload_len);
        return payload_len + 1;
    }
    memcpy(out, payload, payload_len);
    return payload_len;
}

// Read Without Encryption: service (LE on the wire), block numbers list
static int build_rwe(u8 *out, int with_len, const u8 *idm, u16 service,
                     const u8 *blocks, int nblocks)
{
    u8 p[64];
    int n = 0;
    p[n++] = 0x06;
    memcpy(p + n, idm, 8); n += 8;
    p[n++] = 0x01;                       // number of services
    p[n++] = (u8)(service & 0xFF);       // service code, little endian
    p[n++] = (u8)(service >> 8);
    p[n++] = (u8)nblocks;                // number of blocks
    for (int i = 0; i < nblocks; i++) {
        p[n++] = 0x80;                   // 2-byte block list element, service order 0
        p[n++] = blocks[i];
    }
    return build_frame(out, p, n, with_len);
}

// Request Service for one node (service code)
static int build_req_service(u8 *out, int with_len, const u8 *idm, u16 service)
{
    u8 p[16];
    int n = 0;
    p[n++] = 0x02;
    memcpy(p + n, idm, 8); n += 8;
    p[n++] = 0x01;                       // number of nodes
    p[n++] = (u8)(service & 0xFF);
    p[n++] = (u8)(service >> 8);
    return build_frame(out, p, n, with_len);
}

// Request Response
static int build_req_response(u8 *out, int with_len, const u8 *idm)
{
    u8 p[10];
    int n = 0;
    p[n++] = 0x04;
    memcpy(p + n, idm, 8); n += 8;
    return build_frame(out, p, n, with_len);
}

// Polling (system code is big endian on the wire)
static int build_polling(u8 *out, int with_len, u16 system_code)
{
    u8 p[5];
    int n = 0;
    p[n++] = 0x00;
    p[n++] = (u8)(system_code >> 8);
    p[n++] = (u8)(system_code & 0xFF);
    p[n++] = 0x00;                       // time slot: respond immediately
    return build_frame(out, p, n, with_len);
}

// ------------------------------------------------------------------
// Send one raw command and log everything. Returns the service result.
// ------------------------------------------------------------------
static Result send_cmd(const char *label, const u8 *cmd, int cmdlen,
                       u8 *resp, int respcap, int *resp_len)
{
    hexdump(label, cmd, cmdlen);
    memset(resp, 0, respcap);
    size_t actual = 0;
    Result ret = nfcSendTagCommand(cmd, cmdlen, resp, respcap, &actual,
                                   NFC_TIMEOUT_US);
    logprintf("  -> result 0x%08lX, actual size %u\n",
              (unsigned long)ret, (unsigned)actual);
    *resp_len = (int)actual;
    if (R_SUCCEEDED(ret) && actual > 0) {
        int show = actual > 256 ? 256 : (int)actual;
        hexdump("  resp", resp, show);
    }
    return ret;
}

// Parse a Read Without Encryption response: locate the IDm, then
// status flags (2), block count (1), then 16-byte blocks.
// Returns number of blocks parsed, or negative on failure.
static int parse_rwe(const u8 *resp, int len, const u8 *idm,
                     u8 blocks_out[][16], int maxblocks, int *status_out)
{
    for (int i = 0; i + 8 <= len; i++) {
        if (memcmp(resp + i, idm, 8) != 0) continue;
        int p = i + 8;
        if (len - p < 3) return -1;
        int sf = (resp[p] << 8) | resp[p + 1];
        int nb = resp[p + 2];
        if (status_out) *status_out = sf;
        if (sf != 0) return -2;
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

static int idm_is_zero(const u8 *idm)
{
    for (int i = 0; i < 8; i++) if (idm[i]) return 0;
    return 1;
}

// Try to read balance + history with one IDm and one framing variant.
// Returns 1 if the balance block was read and parsed.
static int try_read_card(const u8 *idm, int with_len)
{
    u8 cmd[80];
    u8 resp[0x800];
    int resp_len = 0;
    int success = 0;

    logprintf("\n--- try_read_card: IDm %02X%02X%02X%02X%02X%02X%02X%02X, "
              "framing=%s ---\n",
              idm[0], idm[1], idm[2], idm[3], idm[4], idm[5], idm[6], idm[7],
              with_len ? "WITH length byte" : "WITHOUT length byte");

    // Request Response (cheap sanity check that raw commands work)
    int clen = build_req_response(cmd, with_len, idm);
    send_cmd("cmd Request Response (0x04)", cmd, clen, resp, sizeof(resp), &resp_len);

    // Request Service: does 0x008B exist on this card?
    clen = build_req_service(cmd, with_len, idm, 0x008B);
    send_cmd("cmd Request Service 0x008B", cmd, clen, resp, sizeof(resp), &resp_len);

    // Read Without Encryption: service 0x008B block 0 (attribute / balance)
    {
        u8 blklist[1] = {0};
        clen = build_rwe(cmd, with_len, idm, 0x008B, blklist, 1);
        Result ret = send_cmd("cmd RWE 0x008B block0", cmd, clen,
                              resp, sizeof(resp), &resp_len);
        if (R_SUCCEEDED(ret) && resp_len > 0) {
            static u8 blocks[4][16];
            int status = -1;
            int nb = parse_rwe(resp, resp_len, idm, blocks, 4, &status);
            logprintf("  parse_rwe: blocks=%d status=0x%04X\n", nb, status);
            if (nb >= 1) {
                hexdump("  0x008B block0", blocks[0], 16);
                unsigned bal_a = blocks[0][10] | (blocks[0][11] << 8);
                unsigned bal_b = blocks[0][11] | (blocks[0][12] << 8);
                logprintf("  >> BALANCE candidate A (bytes10-11 LE): %u yen\n", bal_a);
                logprintf("  >> BALANCE candidate B (bytes11-12 LE): %u yen\n", bal_b);
                success = 1;
            }
        }
    }

    // Read Without Encryption: service 0x090F blocks 0..9 (history)
    {
        u8 blklist[10];
        for (int i = 0; i < 10; i++) blklist[i] = (u8)i;
        clen = build_rwe(cmd, with_len, idm, 0x090F, blklist, 10);
        Result ret = send_cmd("cmd RWE 0x090F blocks0-9", cmd, clen,
                              resp, sizeof(resp), &resp_len);
        if (R_SUCCEEDED(ret) && resp_len > 0) {
            static u8 blocks[12][16];
            int status = -1;
            int nb = parse_rwe(resp, resp_len, idm, blocks, 12, &status);
            logprintf("  parse_rwe(history): blocks=%d status=0x%04X\n", nb, status);
            if (nb >= 1) {
                for (int i = 0; i < nb; i++) {
                    const u8 *b = blocks[i];
                    unsigned date = (b[4] << 8) | b[5];
                    unsigned year = 2000 + ((date >> 9) & 0x7F);
                    unsigned mon  = (date >> 5) & 0x0F;
                    unsigned day  = date & 0x1F;
                    unsigned bal  = b[10] | (b[11] << 8);
                    logprintf("  hist[%d] %04u-%02u-%02u proc=0x%02X "
                              "balance_after=%u yen\n",
                              i, year, mon, day, b[1], bal);
                    hexdump("    raw", b, 16);
                }
            }
        }
    }
    return success;
}

// Try a Polling command through SendTagCommand; if the card answers,
// the response is [.. 0x01, IDm(8), PMm(8) ..]. Extract the IDm.
// Returns 1 and fills out_idm on success.
static int try_polling(int with_len, u16 system_code, u8 *out_idm)
{
    u8 cmd[16];
    u8 resp[0x800];
    int resp_len = 0;
    int clen = build_polling(cmd, with_len, system_code);
    logprintf("\n--- try_polling: system=0x%04X framing=%s ---\n",
              system_code, with_len ? "WITH length" : "WITHOUT length");
    Result ret = send_cmd("cmd Polling (0x00)", cmd, clen, resp, sizeof(resp), &resp_len);
    if (R_FAILED(ret) || resp_len < 10) return 0;
    // search for a plausible polling response: byte 0x01 followed by data
    for (int i = 0; i + 17 <= resp_len; i++) {
        if (resp[i] == 0x01) {
            memcpy(out_idm, resp + i + 1, 8);
            if (!idm_is_zero(out_idm)) {
                logprintf("  polling IDm candidate at offset %d\n", i);
                return 1;
            }
        }
    }
    return 0;
}

// ------------------------------------------------------------------
// Dump phase: runs once a tag is InRange under some scanning mask.
// Returns 1 if a balance was read.
// ------------------------------------------------------------------
static int dump_phase(void)
{
    int success = 0;

    svcSleepThread(200000000LL); // let the tag settle, 200ms

    // --- GetTagInfo ---
    NFC_TagInfo ti;
    memset(&ti, 0, sizeof(ti));
    Result ret = nfcGetTagInfo(&ti);
    logprintf("\nnfcGetTagInfo -> 0x%08lX\n", (unsigned long)ret);
    if (R_SUCCEEDED(ret)) {
        logprintf("  id_offset_size=0x%04X unk_x2=0x%02X unk_x3=0x%02X\n",
                  ti.id_offset_size, ti.unk_x2, ti.unk_x3);
        hexdump("  TagInfo.id[0x28]", ti.id, 0x28);
        // Full raw struct too, in case the layout differs for FeliCa.
        hexdump("  TagInfo raw struct", (const u8 *)&ti, sizeof(ti));
    }

    // --- GetTagInfo2 via raw IPC (cmd 0x00100000), undocumented struct ---
    {
        u32 *cmdbuf = getThreadCommandBuffer();
        cmdbuf[0] = IPC_MakeHeader(0x10, 0, 0);
        ret = svcSendSyncRequest(nfcGetSessionHandle());
        logprintf("\nGetTagInfo2 raw IPC: svc ret 0x%08lX, result 0x%08lX\n",
                  (unsigned long)ret, (unsigned long)cmdbuf[1]);
        if (ret == 0) {
            hexdump("  TagInfo2 reply words", (const u8 *)&cmdbuf[2], 0x60);
        }
    }

    // --- Candidate IDms ---
    u8 cand[3][8];
    int ncand = 0;
    memset(cand, 0, sizeof(cand));
    // candidate 0: TagInfo.id[0..7]
    memcpy(cand[ncand], ti.id, 8); ncand++;
    // candidate 1: if id_offset_size > 10, ID data lives at
    // structstart+4+(size-10); copy 8 bytes from there.
    if (ti.id_offset_size > 10) {
        const u8 *base = (const u8 *)&ti;
        int off = 4 + (ti.id_offset_size - 10);
        if (off + 8 <= (int)sizeof(ti)) {
            memcpy(cand[ncand], base + off, 8);
            if (memcmp(cand[ncand], cand[0], 8) != 0) ncand++;
            else memset(cand[ncand], 0, 8);
        }
    }

    // --- Polling attempts (may also reveal the IDm) ---
    u8 poll_idm[8];
    int have_poll_idm = 0;
    if (try_polling(1, 0x0003, poll_idm)) have_poll_idm = 1;
    if (!have_poll_idm && try_polling(1, 0xFFFF, poll_idm)) have_poll_idm = 1;
    if (!have_poll_idm && try_polling(0, 0x0003, poll_idm)) have_poll_idm = 1;
    if (have_poll_idm) {
        logprintf("polling gave IDm %02X%02X%02X%02X%02X%02X%02X%02X\n",
                  poll_idm[0], poll_idm[1], poll_idm[2], poll_idm[3],
                  poll_idm[4], poll_idm[5], poll_idm[6], poll_idm[7]);
        int dup = 0;
        for (int i = 0; i < ncand; i++)
            if (memcmp(cand[i], poll_idm, 8) == 0) dup = 1;
        if (!dup && ncand < 3) { memcpy(cand[ncand], poll_idm, 8); ncand++; }
    }

    // --- Read attempts for each candidate IDm, both framings ---
    for (int c = 0; c < ncand && !success; c++) {
        if (idm_is_zero(cand[c])) {
            logprintf("\ncandidate %d is all-zero, skipping reads\n", c);
            continue;
        }
        if (try_read_card(cand[c], 1)) success = 1;
        else if (try_read_card(cand[c], 0)) success = 1;
    }

    // --- The two undocumented RawNFC commands Megami Meguri also used.
    // Called last so they can't disturb the read attempts above. ---
    ret = nfcCmd21();
    logprintf("\nnfcCmd21 -> 0x%08lX\n", (unsigned long)ret);
    ret = nfcCmd22();
    logprintf("nfcCmd22 -> 0x%08lX\n", (unsigned long)ret);

    return success;
}

// ------------------------------------------------------------------
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    g_log = fopen("sdmc:/icpoc_log.txt", "a");
    if (g_log) fprintf(g_log, "\n==== IC Balance PoC run ====\n");

    logprintf("IC Balance PoC (read-only, no payment)\n");
    logprintf("======================================\n\n");
    logprintf("Put your IC card (Suica/PASMO/ICOCA...) on the\n");
    logprintf("LOWER screen and keep it there during the scan.\n\n");
    logprintf("A: start scan sweep   B: cancel   START: exit\n");
    logprintf("Log file: sdmc:/icpoc_log.txt\n");

    // wait for A
    int start = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_A) { start = 1; break; }
        if (k & KEY_START) break;
        pump_gfx();
        gspWaitForVBlank();
    }

    if (start) {
        Result ret = nfcInit(NFC_OpType_RawNFC);
        logprintf("\nnfcInit(RawNFC) -> 0x%08lX\n", (unsigned long)ret);
        if (R_FAILED(ret)) {
            logprintf("NFC init failed. RawNFC needs system >= 10.0.0\n");
            logprintf("and a New 3DS. Nothing else to try.\n");
        } else {
            // Sweep the StartOtherTagScanning bitmask. 0x01 is known
            // (Type-A / amiibo, from Thenaya). FeliCa's bit is unknown,
            // so likely candidates come first, then the rest.
            static const u8 masks[] = {
                0x04, 0x0F, 0x01, 0x02, 0x08,
                0x03, 0x05, 0x06, 0x07, 0x09,
                0x0A, 0x0B, 0x0C, 0x0D, 0x0E
            };
            int detected_any = 0;
            int success = 0;

            for (unsigned mi = 0; mi < sizeof(masks) && !success; mi++) {
                if (g_user_cancel) break;
                u8 mask = masks[mi];
                logprintf("\n[mask 0x%02X] StartOtherTagScanning...\n", mask);
                ret = nfcStartOtherTagScanning(NFC_STARTSCAN_DEFAULTINPUT, mask);
                if (R_FAILED(ret)) {
                    logprintf("  start failed: 0x%08lX\n", (unsigned long)ret);
                    continue;
                }
                int inrange = 0;
                for (int t = 0; t < 80; t++) { // ~4 seconds per mask
                    NFC_TagState st;
                    if (R_SUCCEEDED(nfcGetTagState(&st))) {
                        if (st == NFC_TagState_InRange) { inrange = 1; break; }
                        if (st == NFC_TagState_OutOfRange) break;
                    }
                    if (wait_ms(50)) break;
                }
                if (g_user_cancel) { nfcStopScanning(); break; }
                if (inrange) {
                    detected_any = 1;
                    logprintf("  *** TAG DETECTED with mask 0x%02X ***\n", mask);
                    success = dump_phase();
                    logprintf("\n[dump done for mask 0x%02X, success=%d]\n",
                              mask, success);
                } else {
                    logprintf("  no tag.\n");
                }
                nfcStopScanning();
                wait_ms(200);
            }

            logprintf("\n======================================\n");
            if (success) {
                logprintf("RESULT: BALANCE READ OK. PoC succeeded!\n");
            } else if (detected_any) {
                logprintf("RESULT: tag was detected, but FeliCa reads\n");
                logprintf("did not parse. Please send icpoc_log.txt back\n");
                logprintf("for the next iteration.\n");
            } else {
                logprintf("RESULT: no tag detected under any mask.\n");
                logprintf("Check: card on lower screen the whole time,\n");
                logprintf("New 3DS (not Old), and no other app using NFC.\n");
            }
            nfcExit();
        }
    }

    logprintf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        pump_gfx();
        gspWaitForVBlank();
    }

    if (g_log) fclose(g_log);
    gfxExit();
    return 0;
}
