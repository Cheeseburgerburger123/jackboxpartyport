//! Functions the C host provides (wasm imports from module "rw").
//!
//! Everything crossing the boundary is u32 words (f32 passed as bits) or byte ranges, so the host can
//! read wasm memory with wasm2c's MEM_ADDR on the big-endian Wii U without caring about Rust layouts.

#[link(wasm_import_module = "rw")]
unsafe extern "C" {
    /// Vertex words: x (f32 bits), y (f32 bits), rgba (r << 24 | g << 16 | b << 8 | a), premultiplied.
    pub fn host_mesh_create(verts: *const u32, nverts: u32, indices: *const u32, nidx: u32) -> u32;
    pub fn host_mesh_free(id: u32);
    /// Premultiplied RGBA8 rows, `stride` bytes apart. `rgba` may be null (texture starts transparent).
    pub fn host_tex_create(w: u32, h: u32, rgba: *const u8, stride: u32) -> u32;
    pub fn host_tex_update(id: u32, x: u32, y: u32, w: u32, h: u32, rgba: *const u8, stride: u32);
    /// A smaller version of a Stage3D texture, for mipmapping (level 1 is half the size, and so on).
    pub fn host_tex_mip(id: u32, level: u32, w: u32, h: u32, rgba: *const u8, stride: u32);
    pub fn host_tex_free(id: u32);
    /// Reads `w`x`h` premultiplied RGBA8 from texture `id` at (x, y) into `out` (stride w*4).
    pub fn host_tex_read(id: u32, x: u32, y: u32, w: u32, h: u32, out: *mut u8);
    /// Runs a filter on the GPU (see filters::gpu_params): texels (sx, sy, sw, sh) of `src` into `dst` at
    /// (dx, dy). Returns 0 if the host couldn't (then it's done on the CPU).
    pub fn host_h264_open(w: u32, h: u32) -> u32;
    pub fn host_h264_decode(decoder: u32, tex: u32, data: *const u8, len: u32) -> u32;
    pub fn host_h264_close(decoder: u32);
    pub fn host_tex_copy(dst: u32, src: u32);
    pub fn host_tex_create_float(w: u32, h: u32, rgba: *const f32) -> u32;
    pub fn host_tex_read_float(id: u32, w: u32, h: u32, out: *mut f32);
    pub fn host_tex_create_float_target(w: u32, h: u32) -> u32;
    pub fn host_pb_run(
        program: u32, target: u32, textures: *const u32, floats: *const f32, nfloats: u32, ints: *const i32, nints: u32,
        zero_outside: u32, linear_mask: u32,
    ) -> u32;
    pub fn host_blend_layer(target: u32, layer: u32, mode: u32, mask_state: u32, mask_depth: u32) -> u32;
    pub fn host_filter(
        kind: u32, src: u32, sx: u32, sy: u32, sw: u32, sh: u32, dst: u32, dx: i32, dy: i32, params: *const f32,
        nparams: u32,
    ) -> u32;
    /// Runs a command list (see `Op`). `target` is 0 for the screen or a texture id to render into.
    pub fn host_frame(target: u32, cmds: *const u32, nwords: u32);
    pub fn host_log(level: u32, msg: *const u8, len: u32);

    // ---- network (host/wiiu/net_wiiu.c) ----
    /// Opens a TCP connection (TLS if `tls` is 1) on a host thread; 0 if it couldn't start.
    pub fn host_sock_open(host: *const u8, host_len: u32, port: u32, tls: u32) -> u32;
    /// 0 connecting, 1 open, 2 failed, 3 closed by the other side (received data can still be read).
    pub fn host_sock_state(id: u32) -> u32;
    /// Queues bytes to send; returns how many were taken (all, or 0 on failure).
    pub fn host_sock_send(id: u32, data: *const u8, len: u32) -> u32;
    /// Takes up to `cap` received bytes; 0 if none are waiting.
    pub fn host_sock_recv(id: u32, buf: *mut u8, cap: u32) -> u32;
    pub fn host_sock_close(id: u32);

    // ---- Stage3D (see context3d.rs) ----
    /// Bit 0: flip Y / map depth 0..1 to -1..1 in the GLSL (OpenGL targets);
    /// bit 1: program constants as plain uniform arrays instead of uniform blocks (GX2 register mode).
    pub fn host_c3d_flags() -> u32;
    /// GPU buffer for vertex (`vertex` = 1: 32-bit values) or index data (0: 16-bit values), uploaded
    /// as the little-endian bytes Flash writes.
    pub fn host_c3d_buf_create(size: u32, vertex: u32) -> u32;
    pub fn host_c3d_buf_upload(id: u32, offset: u32, data: *const u8, len: u32);
    pub fn host_c3d_buf_free(id: u32);
    /// GLSL vertex + fragment shader; 0 if it doesn't compile.
    pub fn host_c3d_program(vs: *const u8, vs_len: u32, fs: *const u8, fs_len: u32) -> u32;
    pub fn host_c3d_program_free(id: u32);
    /// Clears texture `target`; mask bit 0 color, 1 depth, 2 stencil.
    pub fn host_c3d_clear(target: u32, r: u32, g: u32, b: u32, a: u32, depth: u32, stencil: u32, mask: u32);
    /// One draw call with all of its state, see `draw_words` in context3d.rs.
    pub fn host_c3d_draw(words: *const u32, n: u32);
}

/// Command list opcodes. Each op is followed by its words.
pub mod op {
    /// clear: rgba f32 x4
    pub const CLEAR: u32 = 1;
    /// draw: mesh, first_index, index_count, prim, world[6], mult[4], add[4], kind, kind payload
    ///   kind 0 color: nothing
    ///   kind 1 bitmap: tex, flags (1 smooth, 2 repeat), uv matrix[6]
    ///   kind 2 gradient: ramp tex, gradient type, spread, focal f32, uv matrix[6]
    pub const DRAW: u32 = 2;
    /// mask state: state (0 none, 1 draw stencil, 2 draw masked content, 3 clear stencil), mask depth
    pub const MASK: u32 = 3;
    /// blend: Flash blend mode number (swf::BlendMode as u8)
    pub const BLEND: u32 = 4;
}

/// `op::BLEND` value (not a Flash blend mode): write the source unblended (Stage3D layers).
pub const BLEND_OPAQUE: u32 = 255;

pub mod prim {
    pub const TRIANGLES: u32 = 0;
    pub const LINES: u32 = 1;
    pub const LINE_LOOP: u32 = 2;
}

pub fn log(level: u32, msg: &str) {
    unsafe { host_log(level, msg.as_ptr(), msg.len() as u32) }
}
