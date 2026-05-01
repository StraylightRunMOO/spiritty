#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <GLES3/gl3.h>
#include "terminal.h"

namespace spiritty {

// Forward declarations
class Terminal;
class TerminalBuffer;

// WebGL/GLES3 renderer configuration
struct RendererOptions {
    bool antialias = true;
    bool alpha = false;
    bool premultiplied_alpha = true;
    bool preserve_drawing_buffer = false;
    bool fail_if_major_performance_caveat = false;
    int max_texture_size = 2048;
    int atlas_size = 1024;
    int glyph_padding = 2;
    bool use_linear_blending = true;
    bool use_subpixel_antialiasing = false;
    float gamma = 1.8f;
    float contrast = 1.0f;
};

// Glyph atlas entry
struct GlyphAtlasEntry {
    uint32_t glyph_id;
    int x, y;
    int width, height;
    int bearing_x, bearing_y;
    float advance;
    bool is_color;
};

// Render batch for efficient drawing
struct RenderBatch {
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    int vertex_count = 0;
    int index_count = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
};

// WebGL/GLES3 based GPU renderer
class WebGLRenderer {
public:
    explicit WebGLRenderer(Terminal* terminal);
    ~WebGLRenderer();
    
    // Initialization
    bool initialize(const RendererOptions& options);
    void shutdown();
    bool initialized() const { return initialized_; }
    
    // Rendering
    void render();
    void render_line(int row);
    void render_cursor();
    void render_selection();
    
    // Viewport management
    void resize(int width, int height);
    void set_viewport(int x, int y, int width, int height);
    
    // Font management
    void load_font(const std::string& font_family, int font_size, bool bold = false, bool italic = false);
    void unload_font();
    void update_font_metrics();
    
    // Glyph atlas management
    void create_glyph_atlas();
    void update_glyph_atlas();
    void bind_glyph_atlas();
    
    // Texture management
    void create_textures();
    void update_textures();
    void bind_textures();
    
    // Shader management
    void create_shaders();
    void update_uniforms();
    
    // Buffer management
    void create_buffers();
    void update_buffers();
    void bind_buffers();
    
    // Clear operations
    void clear();
    void clear_line(int row);
    void clear_range(int start_row, int start_col, int end_row, int end_col);
    
    // Dirty tracking
    void mark_dirty(int row);
    void mark_all_dirty();
    void clear_dirty_flags();
    bool is_dirty(int row) const;
    
    // Performance optimizations
    void begin_frame();
    void end_frame();
    void flush();
    
    // Texture and buffer updates
    void update_cell_texture(int row, int col, const Cell& cell);
    void update_background_texture();
    void update_cursor_texture();
    
    // Selection rendering
    void render_selection_range(const Selection& selection);
    
    // Mouse interaction
    void screen_to_cell_coords(int screen_x, int screen_y, int& row, int& col);
    void cell_to_screen_coords(int row, int col, int& screen_x, int& screen_y);
    
    // Getters
    int screen_width() const { return screen_width_; }
    int screen_height() const { return screen_height_; }
    int cell_width() const { return cell_width_; }
    int cell_height() const { return cell_height_; }
    const RendererOptions& options() const { return options_; }
    
    // Debug methods
    void dump_state() const;
    void validate_state() const;
    
    // Shader sources (inlined for simplicity)
    static const char* get_vertex_shader_source();
    static const char* get_fragment_shader_source();
    static const char* get_bg_vertex_shader_source();
    static const char* get_bg_fragment_shader_source();
    static const char* get_cursor_vertex_shader_source();
    static const char* get_cursor_fragment_shader_source();

private:
    Terminal* terminal_;
    TerminalBuffer* buffer_;
    RendererOptions options_;
    
    // OpenGL context and resources
    GLuint program_;
    GLuint bg_program_;
    GLuint cursor_program_;
    
    // Vertex Array Objects
    GLuint vao_;
    GLuint bg_vao_;
    GLuint cursor_vao_;
    
    // Vertex Buffer Objects
    GLuint vbo_;
    GLuint bg_vbo_;
    GLuint cursor_vbo_;
    
    // Element Buffer Objects
    GLuint ebo_;
    GLuint bg_ebo_;
    GLuint cursor_ebo_;
    
    // Textures
    GLuint glyph_atlas_;
    GLuint cell_texture_;
    GLuint bg_texture_;
    GLuint cursor_texture_;
    
    // Uniform locations
    GLint u_projection_matrix_;
    GLint u_cell_size_;
    GLint u_glyph_atlas_;
    GLint u_cell_texture_;
    GLint u_bg_texture_;
    GLint u_cursor_texture_;
    GLint u_cursor_pos_;
    GLint u_cursor_style_;
    GLint u_cursor_blink_;
    GLint u_selection_active_;
    GLint u_selection_start_;
    GLint u_selection_end_;
    GLint u_time_;
    
    // Screen dimensions
    int screen_width_;
    int screen_height_;
    int cell_width_;
    int cell_height_;
    
    // Font metrics
    struct FontMetrics {
        int ascent;
        int descent;
        int height;
        int line_gap;
    } font_metrics_;
    
    // Glyph cache
    std::unordered_map<uint32_t, GlyphAtlasEntry> glyph_cache_;
    std::vector<uint8_t> glyph_atlas_data_;
    int atlas_cursor_x_;
    int atlas_cursor_y_;
    int atlas_line_height_;
    
    // Render batches
    std::vector<RenderBatch> batches_;
    RenderBatch* current_batch_;
    
    // Dirty tracking
    std::vector<bool> dirty_lines_;
    bool full_redraw_needed_;
    bool initialized_;
    
    // Performance counters
    uint64_t frame_count_;
    double last_frame_time_;
    
    // Private methods
    bool setup_gl_state();
    void cleanup_gl_state();
    
    void create_glyph_atlas_texture();
    void update_glyph_atlas_texture();
    
    void create_cell_texture();
    void update_cell_texture_region(int start_row, int start_col, int end_row, int end_col);
    
    void create_background_texture();
    void update_background_texture_region(int start_row, int end_row);
    
    void create_cursor_texture();
    
    void upload_vertices();
    void upload_indices();
    
    void build_projection_matrix(float* matrix);
    void build_orthographic_matrix(float* matrix, float left, float right, float bottom, float top, float near, float far);
    
    void render_cells();
    void render_background();
    void render_cursor_blink();
    
    void prepare_batch();
    void flush_batch();
    
    // Glyph rendering helpers
    void render_glyph(uint32_t glyph_id, int row, int col, const CellAttributes& attrs);
    void render_background_cell(int row, int col, uint32_t bg_color);
    
    // Shader compilation
    GLuint compile_shader(GLenum type, const char* source);
    GLuint link_program(GLuint vertex_shader, GLuint fragment_shader);
    
    // Font and text helpers
    uint32_t get_glyph_id(char32_t codepoint, bool bold, bool italic);
    const GlyphAtlasEntry* get_glyph_atlas_entry(uint32_t glyph_id);
    
    // Color helpers
    void premultiply_alpha(uint8_t* rgba);
    uint32_t apply_gamma(uint32_t color, float gamma);
    
    // Dirty flag management
    void mark_line_dirty(int row);
    void mark_region_dirty(int start_row, int start_col, int end_row, int end_col);
    
    // Render state validation
    bool validate_render_state() const;
    
    // Non-copyable
    WebGLRenderer(const WebGLRenderer&) = delete;
    WebGLRenderer& operator=(const WebGLRenderer&) = delete;
};

// Inline shader sources
inline const char* WebGLRenderer::get_vertex_shader_source() {
    return R"(
#version 300 es

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_tex_coord;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_bg_color;
layout(location = 4) in float a_glyph_id;

uniform mat4 u_projection_matrix;
uniform vec2 u_cell_size;
uniform vec2 u_screen_size;

out vec2 v_tex_coord;
out vec4 v_color;
out vec4 v_bg_color;
flat out float v_glyph_id;

void main() {
    vec2 position = a_position * u_cell_size;
    vec2 screen_pos = (position / u_screen_size) * 2.0 - 1.0;
    screen_pos.y = -screen_pos.y;
    
    gl_Position = u_projection_matrix * vec4(screen_pos, 0.0, 1.0);
    v_tex_coord = a_tex_coord;
    v_color = a_color;
    v_bg_color = a_bg_color;
    v_glyph_id = a_glyph_id;
}
)";
}

inline const char* WebGLRenderer::get_fragment_shader_source() {
    return R"(
#version 300 es

precision mediump float;

in vec2 v_tex_coord;
in vec4 v_color;
in vec4 v_bg_color;
flat in float v_glyph_id;

uniform sampler2D u_glyph_atlas;
uniform sampler2D u_cell_texture;

out vec4 frag_color;

void main() {
    // Sample glyph texture
    float alpha = texture(u_glyph_atlas, v_tex_coord).r;
    
    // Apply glyph alpha to color
    vec4 text_color = vec4(v_color.rgb, v_color.a * alpha);
    
    // Blend with background
    vec4 bg_sample = texture(u_cell_texture, gl_FragCoord.xy);
    vec4 bg_color = mix(v_bg_color, bg_sample, bg_sample.a);
    
    // Final composition
    frag_color = mix(bg_color, text_color, text_color.a);
}
)";
}

inline const char* WebGLRenderer::get_bg_vertex_shader_source() {
    return R"(
#version 300 es

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;

uniform mat4 u_projection_matrix;

out vec4 v_color;

void main() {
    gl_Position = u_projection_matrix * vec4(a_position, 0.0, 1.0);
    v_color = a_color;
}
)";
}

inline const char* WebGLRenderer::get_bg_fragment_shader_source() {
    return R"(
#version 300 es

precision mediump float;

in vec4 v_color;

out vec4 frag_color;

void main() {
    frag_color = v_color;
}
)";
}

inline const char* WebGLRenderer::get_cursor_vertex_shader_source() {
    return R"(
#version 300 es

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;

uniform mat4 u_projection_matrix;
uniform vec2 u_cursor_pos;
uniform vec2 u_cell_size;
uniform int u_cursor_style;
uniform float u_blink_alpha;

out vec4 v_color;

void main() {
    vec2 cursor_pos = u_cursor_pos * u_cell_size;
    vec2 position = a_position + cursor_pos;
    
    // Apply cursor style (block, underline, bar)
    if (u_cursor_style == 1) { // underline
        position.y += u_cell_size.y * 0.85;
    } else if (u_cursor_style == 2) { // bar
        position.x += u_cell_size.x * 0.1;
    }
    
    gl_Position = u_projection_matrix * vec4(position, 0.0, 1.0);
    v_color = vec4(a_color.rgb, a_color.a * u_blink_alpha);
}
)";
}

inline const char* WebGLRenderer::get_cursor_fragment_shader_source() {
    return R"(
#version 300 es

precision mediump float;

in vec4 v_color;

out vec4 frag_color;

void main() {
    frag_color = v_color;
}
)";
}

} // namespace spiritty