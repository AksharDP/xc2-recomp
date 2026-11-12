// Frame recording and offline replay of the Vulkan back end (replay_rec.c, replay.c).
//
// A capture is the renderer's input, not the game's state: the bytes and objects the back end holds at one present, and
// the calls the game makes after it. The replay runs those calls on the back end of the current build, so a rendering
// bug at a point deep in the game is reproduced in seconds, with no game code, input or waiting. It is not a savestate.
// The game cannot be resumed from one: its threads' call chains live on host stacks and it holds Vulkan and system
// handles, none of which another process can take over. A change to the game's logic needs a live run; a change to the
// back end (vk_*.c, the shader translator) is what a replay tests.
//
// The back end's calls are journaled from the start, and nothing is written until a capture is asked for. Every capture
// gets a folder of its own under <dir>, which is captures/ beside xc2.exe or XC2_RECORD=<dir> (a relative one is beside xc2.exe too):
//   <dir>/frame-N/   one frame: the state frame N starts from, and frame N's calls
//   <dir>/clip-N/    the same state, then every frame from N on until the capture is stopped
// with log.bin (the record), info.txt (what was captured, written at the first present) and live-<n>.png (the picture
// the game presented: every present of a frame capture, the first of a clip). Ways to ask:
//   F1 menu, Advanced tab (rec_menu.h): Capture frame; Record frames, which runs until Stop recording.
//   XC2_RECORD_FROM=N: frame N, without the menu. With XC2_RECORD_FRAMES=<n> above 1, a clip of n frames from N.
//   XC2_RECORD_ARM_FILE=<path>: the frame after the first present that finds the file (the file is removed), so a
//   script or a person can ask at any moment of a run. XC2_RECORD_FRAMES=<n> makes it a clip here too.
// XC2_RECORD_SHOTS=<dir> also saves every recorded present as <dir>/p<n>.png.
//
// A capture starts (rec_arm) after the present it is asked at, with a preamble of the state the next frame starts from:
// the heap's non-zero bytes, the module images, the device modes, pools, texture slots, samplers, and every level and
// layer of the images whose content the guest bytes do not hold (RR_IMAGE). From there it logs every call the front end
// makes into the back end (gfx_backend), with the guest memory the GPU can read: before each submission and each pool
// call, the heap pages the CPU wrote since the previous one (vk_mem_record_pages), and every range the game gives back
// (memory.c decommit). Freezing is held off while a capture runs (vk_mem_capture_hold): a frozen page's first write
// does not show in the write-watch.
//
// xc2.exe --replay <dir>/frame-N: the same calls on the current code, with no game and no window. Before each recorded
// call the memory records written since the one before are put back at their addresses, then the call is made. A
// present reads its texture back, compares it with the hash the record took, and saves it as replay-<n>.png next to
// live-<n>.png (XC2_REPLAY_SHOTS=<dir> saves it as <dir>/p<n>.png instead; XC2_REPLAY_SHOTS=0 saves nothing). The
// replay runs the back end synchronously (XC2_REPLAY_ASYNC=0) and waits for every shader and pipeline, so it is
// deterministic up to the GPU's own arithmetic. XC2_REPLAY_FRAMES=<n> stops after n presents. XC2_REPLAY_VERIFY=1:
// each WriteTexels is read back, and every written image is compared at each present.
//
// The record is only as good as the state it captures. Not in it: guest memory outside the heap block and the module
// images (stacks, the alias region), writes to module data after the capture starts, host-owned storage of virtual and
// physical pools, images that cannot be read back (multisampled, packed depth-stencil, ASTC; rec_arm logs how many),
// and state the back end keeps from earlier submissions. The replay compares every presented picture, so a missing
// piece shows as a mismatch rather than as a silent difference.
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
//   NVN_STATE   u64 depth_minus_w, u64 origin_upper_left: the device modes the game set through DeviceSetDepthMode and
//               DeviceSetWindowOriginMode (nvn_api_core.c). The back end reads them on every draw, and the calls themselves are
//               not in the record, so the preamble carries their values and the replay sets them (nvn_replay_state).
//   IMAGE       tex_t, view_t, NVNcopyRegion, u32 owned, u32 0, u64 n, n bytes: one level and layer of an image as it is
//               when the capture starts (vk_texture_snapshot), in tight rows; the view names the level and the layer, the
//               region the part's extent. The replay writes the texels into the image itself (vk_texture_restore) and,
//               with owned set, makes the image GPU-owned as it was, so that no upload from its storage replaces them.
//   The record starts with a COMMIT of the heap's committed part, for what the game committed before the recorder began.
enum {
    RR_DELTA = 1, RR_DECOMMIT, RR_POOL_INIT, RR_POOL_FINI, RR_POOL_MAP, RR_TEX_REG, RR_SAMP_REG, RR_TEX_REL, RR_PROG_REL,
    RR_SUBMIT, RR_PRESENT, RR_FINISH, RR_READ, RR_WRITE, RR_GUEST, RR_CPU_SYNCED, RR_COMMIT, RR_CMD, RR_PIN, RR_NVN_STATE,
    RR_IMAGE, RR_N
};

#define REC_MAGIC "XC2REC01"
#define REC_VERSION 7   // 2: TEX_REG has_view; 3: COMMIT; 4: CMD (command blocks are host memory); 5: PIN (texture pins);
                        // 6: NVN_STATE (the device's depth mode and window origin, in the preamble); 7: IMAGE (every
                        // level and layer of the images the guest bytes do not hold, in place of level 0 as WRITE)
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
// Saves a picture from rec_picture as a PNG (BGRA is saved as RGBA; other formats are not saved). px is left as it is.
void rec_save_picture(const char *path, const tex_t *t, const uint8_t *px, size_t n);

// XC2_REC_TRACE=1 (record and replay): each submission's first words go to stderr, to compare the two runs' memory.
void rec_trace_submit(const char *who, const uint64_t *handles, int n);

// xc2.exe --replay <dir>: 0 when every presented picture matches the record, 1 otherwise (or on a bad record).
int replay_main(const char *dir);

// Exit status of a probe that could not run because data it needs is absent: the test runner reports it as a skip,
// not a pass (tests/run.py).
#define PROBE_SKIP 77
