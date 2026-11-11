// Frame recording and offline replay of the Vulkan back end (replay_rec.c, replay.c).
//
// The back end's calls are journaled from the start, and nothing is written until a capture is asked for from the F1
// menu's Advanced tab (rec_menu.h): Capture frame writes <dir>/frame-N/log.bin, Capture checkpoint writes
// <dir>/checkpoint-N/log.bin until Stop checkpoint. <dir> is captures/ in the working folder, or XC2_RECORD=<dir> when set.
// A capture logs every call the front end makes into the back end (gfx_backend), with the guest memory the GPU can read:
// before each submission and each pool call, the heap pages the CPU wrote since the previous one (vk_mem_record_pages),
// and every range the game gives back (memory.c decommit). Freezing is held off while a capture runs
// (vk_mem_capture_hold): a frozen page's first write does not show in the write-watch.
// XC2_RECORD_FRAMES=<n> stops the log after the n-th present. XC2_RECORD_SHOTS=<dir> also saves each presented picture
// as <dir>/p<n>.png, read back by the record itself after the frame's work is done.
// A capture starts (rec_arm) after the present it is asked at, with a preamble of the state the next frame starts from (the
// live heap's non-zero bytes, module images, pools, texture slots, samplers and the images the GPU owns). A frame capture
// takes one present. Scripts: XC2_RECORD_FROM=N starts a log.bin after present N-1, and XC2_RECORD_ARM_FILE=<path> at the
// first present after the file is created (the file is removed).
// XC2_REPLAY_VERIFY=1 (replay): each WriteTexels is read back, and every written image is compared at each present.
//
// xc2.exe --replay <dir>: the same calls on the current code, with no game and no window. Before each recorded call
// the memory records written since the one before are put back at their addresses, then the call is made. A present
// reads its texture back and compares it with the hash the record took. XC2_REPLAY_SHOTS=<dir> saves each replayed
// present as <dir>/p<n>.png. The replay runs the back end synchronously (XC2_REPLAY_ASYNC=0), so it is deterministic
// up to the GPU's own arithmetic.
//
// The record is only as good as the state it captures: guest memory outside the heap block (module data, the stack,
// the alias region) and host-owned storage of virtual and physical pools are not in it. The replay compares every
// presented picture, so a missing piece shows as a mismatch rather than as a silent difference.
#pragma once
#include <stdint.h>

#include "gfx.h"

// Record kinds. Payloads, in order (sizes from the header: pool_t, tex_t, view_t, samp_par, prog_t, NVNcopyRegion):
//   DELTA       u64 addr, u64 n, n bytes of guest memory at addr
//   DECOMMIT    u64 addr, u64 n
//   POOL_INIT   u64 obj, pool_t          (the object as the call sees it; the replay writes it at obj first)
//   POOL_FINI   u64 obj, pool_t
//   POOL_MAP    u64 obj, u32 n, u32 0, n requests of 0x28 bytes
//   TEX_REG     u32 id, u32 has_view (0: the call passed NULL for the view, whose 16 bytes are zero), tex_t, view_t
//   SAMP_REG    u32 id, u32 0, samp_par
//   TEX_REL     tex_t
//   PROG_REL    prog_t
//   SUBMIT      u32 n, u32 0, n u64 handles (guest addresses of command streams)
//   PRESENT     tex_t, u32 has_crop, u32 0, i32 crop[4], u64 at_ns, u64 frame (the present's number), u64 hash of the
//               picture (0 when the format has no picture)
//   FINISH      -
//   READ        tex_t, view_t, NVNcopyRegion, u64 row, u64 img, u64 need (bytes the call reads into)
//   WRITE       tex_t, view_t, NVNcopyRegion, u64 row, u64 img, u64 n, n bytes of source texels
//   GUEST       u32 0 written / 1 read, u32 0, u64 addr, u64 size
//   CPU_SYNCED  u64 fence
//   COMMIT      u64 addr, u64 n     (the range the game committed: the replay commits it too, so imports of it work)
//   CMD         u64 addr, u64 n, n bytes: one command block of the submissions that follow. The NVN layer keeps the
//               command streams in host memory (a submitted handle is the address of its copy), so the record takes the
//               bytes of each block a submission reaches: its handles, and the blocks their OP_JUMP and OP_CALL name.
//               A block runs from its first op to its OP_END or OP_JUMP. The replay copies each block to a new address
//               and every 8-byte word that holds the address of another recorded host object is pointed at the copy.
//   PIN         u64 addr, u64 sizeof(tex_t), tex_t: a texture pin (nvn_api_cmd.c tex_pin) at addr, as the record had it.
//               The command blocks point at pins; the replay gives each pin a copy and relocates the words as for CMD.
//   The record starts with a COMMIT of the heap's committed part, for what the game committed before the recorder began.
enum {
    RR_DELTA = 1, RR_DECOMMIT, RR_POOL_INIT, RR_POOL_FINI, RR_POOL_MAP, RR_TEX_REG, RR_SAMP_REG, RR_TEX_REL, RR_PROG_REL,
    RR_SUBMIT, RR_PRESENT, RR_FINISH, RR_READ, RR_WRITE, RR_GUEST, RR_CPU_SYNCED, RR_COMMIT, RR_CMD, RR_PIN, RR_N
};

#define REC_MAGIC "XC2REC01"
#define REC_VERSION 5   // 2: TEX_REG has_view; 3: COMMIT; 4: CMD (command blocks are host memory); 5: PIN (texture pins)
typedef struct {
    char magic[8];
    uint32_t version, sz_pool, sz_tex, sz_view, sz_samp, sz_prog, sz_copy, pad;
    uint64_t heap_base, heap_size;
} RecHeader;
typedef struct {
    uint32_t kind, pad;
    uint64_t size;      // payload bytes after this header
} RecRecord;

// The recorder (replay_rec.c). rec_wrap returns the back end the front end calls: a wrapper that logs every call and
// forwards it to b (always, so that a capture can start at any frame). rec_note_decommit is called by memory.c for each
// range it gives back.
const GfxBackend *rec_wrap(const GfxBackend *b);
void rec_note_decommit(uint64_t addr, uint64_t n);
void rec_note_commit(uint64_t addr, uint64_t n);

// The presented picture: the texels of t read back whole, as ReadTexels returns them, and their FNV-1a hash.
// Returns 0 when the format's block size is unknown (no picture), else the byte count in *px (malloc'd, freed by the
// caller). The record and the replay use this one function, so their hashes agree.
size_t rec_picture(const GfxBackend *b, const tex_t *t, uint8_t **px, uint64_t *hash);
// Saves a picture from rec_picture as a PNG (BGRA swapped to RGBA; other formats are not saved).
void rec_save_picture(const char *path, const tex_t *t, uint8_t *px, size_t n);

// XC2_REC_TRACE=1 (record and replay): each submission's first words go to stderr, to compare the two runs' memory.
void rec_trace_submit(const char *who, const uint64_t *handles, int n);

// xc2.exe --replay <dir>: 0 when every presented picture matches the record, 1 otherwise (or on a bad record).
int replay_main(const char *dir);

// Exit status of a probe that could not run because data it needs is absent: the test runner reports it as a skip,
// not a pass (tests/run.py).
#define PROBE_SKIP 77
