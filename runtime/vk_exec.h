// The op-stream replay of the Vulkan back end. vk_exec.c walks the command
// handles of a submission, keeps render targets, passes, clears and fixed-function state; draws, binds and dispatches
// (vk_exec_draw.c) and copies, counters, events and conditional rendering (vk_exec_xfer.c) plug in through the two
// hooks at the bottom.
// Runs on the submitting thread under vk_sync_submit's lock: no locking inside.
// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>

#include "nvn_api.h"
#include "vk.h"

// Dynamic state the next draw must record (VkExec.dirty). vk_exec_record sets them all at the start of each command
// buffer; the state ops set theirs; vk_exec_draw.c clears what it recorded.
enum {
    VK_EXEC_DIRTY_VIEWPORT = 1 << 0,     // viewports or depth range
    VK_EXEC_DIRTY_SCISSOR = 1 << 1,
    VK_EXEC_DIRTY_DEPTH_BIAS = 1 << 2,   // polygon offset
    VK_EXEC_DIRTY_BLEND_CONST = 1 << 3,
    VK_EXEC_DIRTY_STENCIL = 1 << 4,      // compare and write masks, references
    VK_EXEC_DIRTY_ALL = (1 << 5) - 1,
};

// One render target of OP_SET_RT, resolved to its vk_texture.c image for the current command buffer.
typedef struct {
    const tex_t *tex;                    // texture pin of ex->rt (nvn_pins_sweep keeps it); NULL: no target in this slot
    VkImage image;                       // VK_NULL_HANDLE: no target or not renderable (logged once)
    VkFormat format;                     // view format: the view's when it reinterprets a same-size format
    VkImageViewType view_type;           // 2D, or 2D_ARRAY for more than one layer
    VkImageAspectFlags aspect;
    uint32_t level, base_layer, layers;  // subresource of the view; of a 3D image: depth planes
    uint8_t is3d;                        // a 3D image (colour only), viewed through 2D views of its slices
    uint8_t blocks;                      // a BC image viewed as one texel per block (VkTexImage.block_views)
    uint32_t width, height, samples;     // extent of that level; with blocks, in blocks
    int32_t nvn_format;                  // NVNformat of the view (VkPipeState.color_fmt / depth_fmt)
    const VkTexImage *img;               // the image (fb_usage, fb_learned)
    uint8_t scale;                       // VkTexImage.scale
} VkExecTarget;

void vk_exec_cond_frame_end(uint64_t frame);   // vk_exec_xfer.c: a present, in replay order (NX2NAVI_LOG_RCOND)
extern unsigned vk_cond_clears_run, vk_cond_clears_skip, vk_cond_draws_skip, vk_cond_depth_run;
void vk_exec_cond_why(const p_render_cond *c, char *buf, size_t cap);   // NX2NAVI_LOG_RCOND=ab

// The replay state. One instance lives for the run: NVN state is queue state and carries over from one submission to
// the next. The "per command buffer" fields are reset by vk_exec_record.
typedef struct VkExec {
    // ---- per command buffer
    VkCommandBuffer cb;                  // being recorded
    uint64_t fence;                      // timeline value the submission signals (vk_completion_add)
    int recorded;                        // Vulkan commands recorded so far; every handler adds the ones it records
    int in_pass;                         // inside vkCmdBeginRendering
    uint32_t dirty;                      // VK_EXEC_DIRTY_*
    // Pass marks (vk.h vk_pass_mark_begin): the open pass's record (NULL: none or marks off), vk_exec_frame.draws when
    // it began, and the passes and dispatches this submission marked so far.
    VkPassMark *mark;
    uint64_t mark_draws0;
    uint32_t mark_passes, mark_dispatches;

    // ---- render targets (OP_SET_RT) and the current pass
    p_set_rt rt;                         // last OP_SET_RT payload (a change ends the open pass); rt.n 0, no depth: none
    int rt_resolved;                   // color[] and depth hold this command buffer's images for rt
    VkExecTarget color[8], depth;        // resolved targets
    uint32_t pass_w, pass_h, pass_layers;            // render area and layer count of the open (or next) pass
    uint32_t pass_scale;                             // its targets' VkTexImage.scale: game viewports and scissors
                                                     // are multiplied by it
    int pass_noatt;                                   // the pass has no attachment: its size follows the draws (vk_exec_noatt_dims)
    VkFormat pass_color[8], pass_depth, pass_stencil; // attachment formats of the pass (VkPipelineRenderingCreateInfo);
                                                      // VK_FORMAT_UNDEFINED: none
    uint32_t pass_ncolor, pass_samples;               // colorAttachmentCount and rasterization samples of the pass

    // ---- fixed-function state (vk_exec.c keeps it; vk_exec_draw.c records it before draws)
    float vp[16][6];                     // OP_VIEWPORTS: x, y, w, h, near, far
    int has_vp;                          // a viewport was set; else the render area
    p_scissor scissor;                   // OP_SCISSOR (scissor 0)
    int has_scissor;                     // a scissor was set; else unbounded
    float depth_range[2];                // OP_DEPTH_RANGE: overrides the viewports' near and far
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];   // OP_STENCIL per face (0 front, 1 back): compare mask, write mask, ref
    float point_size;                    // OP_BIND_STATE kind 6: NVN point size for the support buffer; 0 = never set = 1
    p_poly_offset poly_offset;           // OP_POLY_OFFSET: depth bias factor, units, clamp
    p_prim_restart prim_restart;         // OP_PRIM_RESTART: applied to indexed strip and fan draws (op_draw_idx)
    float blend_color[4];                // OP_BLEND_COLOR
    int64_t copy_stride;                 // OP_COPY_STRIDE: row stride of buffer <-> texture copies (vk_exec_xfer.c)
    const dpool_t *tex_pool, *samp_pool; // OP_SET_TEX_POOL, OP_SET_SAMP_POOL
    // Pipeline inputs. vk_exec.c writes depth_clamp (OP_DEPTH_CLAMP), raster_discard (OP_RASTER_DISCARD) and, at pass
    // begin, color_fmt, depth_fmt and samples; vk_exec_draw.c owns every other field (shaders, prim, y_flip, state
    // objects, vertex input).
    VkPipeState pipe;

    // ---- vk_exec_draw.c slots (draws, binds, dispatch): left empty by vk_exec.c
    const prog_t *prog;                  // OP_BIND_PROGRAM
    int32_t prog_stages;
    NVNbufferRange vbuf[16];             // OP_BIND_VBUF
    p_bind_buf ubo[8][18], ssbo[8][16];  // OP_BIND_UBO / OP_BIND_SSBO per NVN stage (6 task, 7 mesh) and index
    uint8_t cs_push[1024];                             // OP_PUSH_CC: NVN2 compute push constants (c0 from 0x10)
    uint64_t tex[8][128], samp[8][128], img[8][32];   // OP_BIND_TEX / OP_BIND_SAMP / OP_BIND_IMG handles per stage

    // ---- vk_exec_xfer.c slots (render enable, conditional rendering): left empty by vk_exec.c
    int render_disabled;                 // OP_RENDER_ENABLE false or a failed OP_RENDER_COND: clears and draws skip
    p_render_cond render_cond;           // last OP_RENDER_COND
    int has_render_cond;

    // ---- internal to vk_exec.c
    void *views;                         // image views created for this command buffer (freed at its fence)
    int nviews, cap_views;
    int barrier_at;                      // recorded right after the last full barrier (-1: none yet)

    // Feedback loops (vk.h VkCtx.feedback, vk_exec_draw.c feedback_prepare). Learned targets are attached in
    // ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT; they stay in it across consecutive passes and go back to GENERAL at the next
    // full barrier that is not a pass begin.
    VkImage fbl_img[9];                  // images in the feedback layout now
    VkImageAspectFlags fbl_aspect[9];
    int nfbl;
    uint32_t fbl_mask;                   // attachments of the open pass in that layout: bit i colour i, bit 8 depth
    uint32_t fb_dirty;                   // attachments of the open pass written by a draw or clear since the pass began
                                         // or the last in-pass barrier
    uint32_t fb_read;                    // attachments of the open pass a draw sampled (feedback layout) since the pass
                                         // began or the last in-pass read-to-write barrier (feedback_war)
    const void *fb_fs;                   // feedback_scan's memo: fragment shader, texture bind serial, pass serial
    uint32_t fb_serial, fb_pass, fb_sampled;
    uint32_t tex_serial, pass_serial;    // bumped by every texture or image bind and pass begin
} VkExec;

// Begins dynamic rendering with the current targets unless a pass is open (a full memory barrier precedes it). Call
// before a draw or an attachment clear.
void vk_exec_pass_begin(VkExec *ex);
// Ends the open pass (no-op without one) and marks its targets GPU-written. Call before copies and dispatches.
void vk_exec_pass_end(VkExec *ex);
// Full memory barrier (all commands, all memory); ends the open pass first. Copies and dispatches order themselves with
// it. Skipped when nothing was recorded since the last one.
void vk_exec_barrier(VkExec *ex);
// vk_texture.c (NX2NAVI_TEX_ALIAS): commands that must run at the replay's current position. vk_exec_inline_begin returns
// the command buffer being recorded after a full barrier (an open pass is ended; *reopen says so), or VK_NULL_HANDLE
// when this thread is not recording a replay. vk_exec_inline_end begins the pass again when it was open.
// vk_exec_inline_buffer: a device buffer for transfers, destroyed when the submission completes.
VkCommandBuffer vk_exec_inline_begin(int *reopen);
void vk_exec_inline_end(int reopen);
// vk_exec_draw.c: forget what the command buffer being recorded has bound (pipeline, sets, dynamic state, vertex
// buffers): the replay continues in a new one (vk_sync_split).
void vk_exec_draw_new_cb(void);
// vk_exec_draw.c, before a part of submission fence goes to the queue (vk_exec.c maybe_split): 1 when it may go. The
// texture pool set it binds is then sealed and the next draw that needs the pools takes another; 0 when every other set
// is bound by this submission's earlier parts, and the replay keeps recording into the same part.
int vk_exec_pool_split(uint64_t fence);
// vk_exec_draw.c: the vertex and fragment shader of the last draw recorded into this command buffer (NULL: none yet).
void vk_exec_draw_shaders(const GfxShader **vs, const GfxShader **fs);
// vk_exec_draw.c "pool marks": at the end of a submission that recorded work, after its closing barrier. Hands the
// mark copies the GPU completed to vk_texture.c and, every few such submissions, copies the marks out and clears them.
void vk_exec_pool_marks(VkExec *ex);
VkBuffer vk_exec_inline_buffer(VkDeviceSize size);
// vk_exec.c: the stand-in a 3D BC render target is drawn through goes with its image (vk_exec_image_gone).
void vk_exec_bc_gone(VkImage image);
// vk_hdr.c: with NX2NAVI_HDR=1, notes a draw into a presented window texture (vk_hdr_track: set by the first present).
extern int vk_hdr_track;
void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info);
// Scissor 0 clipped to the render area of the open pass; extent 0: nothing to draw.
VkRect2D vk_exec_scissor(const VkExec *ex);
// vk_exec_draw.c: orders the open pass's earlier sampled reads of the attachments in written (bit i colour i, bit 8
// depth) before a draw or clear writes them; nothing when none of them was sampled since the last such barrier.
void vk_exec_feedback_war(VkExec *ex, uint32_t written);
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h);   // pass size when no attachment is set
// 1: every target of the current pass (any 1: at least one, and not only the depth target while a colour target is
// rendered for the first time) was rendered in the previous presented frame (per-frame targets); 0 otherwise, or when
// the pass has none.
int vk_exec_targets_recent(VkExec *ex, int any);
// The current pass's targets with their formats, extents and the frames they were last rendered in (diagnostics).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap);

// NX2NAVI_TRACE=gfx: replay counters of the current frame, logged and reset at each present (vk_exec_frame_log).
typedef struct {
    uint64_t passes, draws_seen, draws, clears, copies, dispatches, submits;
    uint64_t fb_draws, fb_barriers, fb_restarts, fb_layouts;   // feedback loops: draws sampling a written target of
                                                               // their pass, in-pass barriers, pass restarts, layout
                                                               // transitions
    uint64_t fb_war_barriers;            // in-pass barriers before a write to a target an earlier draw sampled
    uint64_t ub_updates, ub_pass_breaks; // inline uniform updates, and those that ended an open pass
    uint64_t bind_calls, bind_ns;        // bind_resources: calls and the time spent resolving and pushing their sets
    uint64_t bind_tex_ns;                // of bind_ns, the texture descriptors' lookups
    uint64_t bind_buf_ns;                // of bind_ns, the uniform and storage buffer descriptors' resolution
    uint64_t bind_sig_ns;                // of bind_ns, the per-draw binding signature
    uint64_t bind_push_ns;               // of bind_ns, the set packing, pushes and binds after the binding loop
    uint64_t bind_reused, bind_resolved; // descriptors taken from the per-binding reuse (PB) and resolved again
    uint64_t dyn_calls, dyn_left;        // dynamic state calls recorded, and left out as unchanged (B.dyn)
} VkExecFrame;
extern VkExecFrame vk_exec_frame;
void vk_exec_frame_written(const tex_t *t);                    // vk_texture_gpu_written: t rendered or copied into
uint64_t vk_exec_recording_fence(void);                        // fence of the submission this thread records, else 0
void vk_exec_frame_log(const tex_t *presented, uint64_t frame);
// The draws skipped since the last call because a shader or pipeline was still building (vk_exec_draw.c), and whether
// a frame lacking them would be held back at its present (vk_backend.c frame_hold).
uint32_t vk_exec_pending_draws_take(void);
int vk_frame_hold_left(void);

// NX2NAVI_DUMP=<frame> (vk_dump.c): between presents frame-1 and frame, every pass's targets and every dispatch's storage
// images are copied out and written as PNGs to NX2NAVI_DUMP_DIR (default "dump"), with passes.txt (targets, pipelines,
// textures, samplers) and images.txt (per image value statistics). The hooks run only when vk_dump_armed is set.
extern int vk_dump_armed;
void vk_dump_init(void);                 // reads the environment; vk_exec_record's first call
void vk_dump_pass_end(VkExec *ex);       // after vkCmdEndRendering
// after a draw's binds; mesh: a mesh draw (s->vs is its mesh stage, s->ts its task stage or NULL)
void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info, int mesh);
// Around a mesh draw of the dumped frame (after vk_dump_draw): a primitives-generated query, listed in mesh_prims.txt.
// begin returns the query, or -1 when none was begun; end takes that value.
int vk_dump_mesh_begin(VkExec *ex);
void vk_dump_mesh_end(VkExec *ex, int q);
void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d);
void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload);   // before a copy or texture clear is replayed
void vk_dump_present(uint64_t frame);    // vk_be_present: waits for the dump frame's copies and closes the logs

// c0, the NVN driver constant bank of one stage (vk_exec_draw.c; NX2NAVI_FIX_C0=0 binds zeros instead): bank is
// VK_C0_BYTES, stage an NVN stage (0 vertex, 1 fragment, 5 compute), ssbo and ubo the 16 bound slots of that stage.
#define VK_C0_BYTES 0x800u
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance);

// Handler hooks. vk_exec.c defines both WEAK: they stop with rt_fatal naming the op.
// vk_exec_draw.c: OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BIND_* (program, state, vattr, vstream, vbuf, ubo, ssbo, tex,
// samp, img), OP_UPDATE_UB, OP_DRAW_IDX_IND, OP_DISPATCH_IND. payload: nvn_op_payload of the record.
void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload);
// The aspect a sampled view of img reads (vk_exec_draw.c get_view): stencil for a view in STENCIL depth-stencil mode.
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv);
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks);   // kept for the image's life
// vk_exec_xfer.c: OP_COPY_B2B, OP_COPY_B2T, OP_COPY_T2B, OP_COPY_T2T, OP_CLEAR_TEX, OP_SIGNAL_EVENT, OP_REPORT_COUNTER,
// OP_RESET_COUNTER, OP_RENDER_ENABLE, OP_RENDER_COND, OP_CLEAR_BUF.
void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload);
// OP_WAIT_EVENT (vk_exec_xfer.c "event order check"): counts the wait; 1 when its condition holds in replay order (the
// value in memory, or the signals replayed so far), 0 when its signal has not been replayed yet.
int vk_exec_wait_event(const void *payload);
int vk_exec_event_met(const void *payload);   // the same test without counting
extern int vk_exec_signalled;                 // set by every replayed SignalEvent; vk_exec.c clears it
// Since the last call, which resets them: waits, waits reached before their signal, those of them a later replayed
// signal met, and the sum of how many submissions later it came.
void vk_exec_event_stats(uint64_t *waits, uint64_t *unmet, uint64_t *late, uint64_t *late_subs);
extern uint64_t vk_exec_submit_seq;   // replay thread: submissions replayed so far
int vk_exec_held(void);               // replay thread: held submissions waiting for a signal (event order)
int vk_exec_held_ready(void);         // ... and one of them can go on now (vk_backend.c present_held)
// vk_dlaa.c: once DLSS has scaled the scene this frame, passes larger than the scene's light target vk_dlaa_swap_img
// sample DLSS's output vk_dlaa_swap_view in its place, so the game's scaling draw scales DLSS's picture.
extern const VkTexImage *vk_dlaa_swap_img;
extern VkImageView vk_dlaa_swap_view;
// vk_contact.c: a pass about to begin (its targets resolved, no pass open), and a dispatch or copy about to be recorded
void vk_contact_pass_begin(VkExec *ex);
void vk_contact_flush(VkExec *ex);
// vk_exec_xfer.c, samples-passed counting: a draw about to be recorded in the open pass (begins an occlusion query
// when counting and none is active), and the open pass about to end (ends the active query).
void vk_exec_oq_draw(VkExec *ex);
void vk_exec_oq_pass_end(VkExec *ex);
