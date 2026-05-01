#include "spiritty/webgl_renderer.h"
#include "spiritty/terminal.h"
#include "spiritty/buffer.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace spiritty {

WebGLRenderer::WebGLRenderer(Terminal* terminal) 
    : terminal_(terminal),
      buffer_(nullptr),
      screen_width_(800),
      screen_height_(600),
      cell_width_(8),
      cell_height_(16),
      program_(0),
      bg_program_(0),
      cursor_program_(0),
      vao_(0),
      bg_vao_(0),
      cursor_vao_(0),
      vbo_(0),
      bg_vbo_(0),
      cursor_vbo_(0),
      ebo_(0),
      bg_ebo_(0),
      cursor_ebo_(0),
      glyph_atlas_(0),
      cell_texture_(0),
      bg_texture_(0),
      cursor_texture_(0),
      frame_count_(0),
      last_frame_time_(0.0),
      full_redraw_needed_(true),
      initialized_(false) {
    
    font_metrics_.ascent = 12;
    font_metrics_.descent = 4;
    font_metrics_.height = 16;
    font_metrics_.line_gap = 0;
    
    // Initialize atlas
    atlas_cursor_x_ = 0;
    atlas_cursor_y_ = 0;
    atlas_line_height_ = 0;
}

WebGLRenderer::~WebGLRenderer() {
    shutdown();
}

bool WebGLRenderer::initialize(const RendererOptions& options) {
    options_ = options;
    
    if (!setup_gl_state()) {
        std::cerr << "Failed to setup GL state" << std::endl;
        return false;
    }
    
    create_shaders();
    create_buffers();
    create_textures();
    
    // Initialize glyph atlas
    create_glyph_atlas();
    
    // Set initial viewport
    set_viewport(0, 0, screen_width_, screen_height_);
    
    initialized_ = true;
    return true;
}

void WebGLRenderer::shutdown() {
    cleanup_gl_state();
    initialized_ = false;
}

void WebGLRenderer::render() {
    if (!validate_render_state()) return;
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Render background
    render_background();
    
    // Render cells
    render_cells();
    
    // Render cursor
    render_cursor_blink();
    
    // Render selection
    if (terminal_->selection().active) {
        render_selection_range(terminal_->selection());
    }
    
    frame_count_++;
}

void WebGLRenderer::render_line(int row) {
    if (row < 0 || row >= terminal_->rows()) return;
    
    const TerminalLine& line = buffer_->line(row);
    
    for (int col = 0; col < terminal_->cols(); ++col) {
        const Cell& cell = line.cell(col);
        
        if (!cell.is_empty()) {
            render_glyph(cell.codepoint, row, col, cell.attrs);
        }
    }
}

void WebGLRenderer::render_cursor() {
    if (!terminal_->cursor().visible || !terminal_->cursor().blink_state) return;
    
    const Cursor& cursor = terminal_->cursor();
    int row = cursor.row;
    int col = cursor.col;
    
    // Calculate position
    float x = col * cell_width_;
    float y = row * cell_height_;
    
    // Set cursor color
    uint32_t cursor_color = terminal_->options().color_cursor;
    float r = ((cursor_color >> 16) & 0xFF) / 255.0f;
    float g = ((cursor_color >> 8) & 0xFF) / 255.0f;
    float b = (cursor_color & 0xFF) / 255.0f;
    
    // Render cursor based on style
    int style = terminal_->options().cursor_style;
    
    glUseProgram(cursor_program_);
    
    // Set uniforms
    glUniform2f(u_cursor_pos_, col, row);
    glUniform1i(u_cursor_style_, style);
    glUniform1f(u_cursor_blink_, terminal_->cursor().blink_state ? 1.0f : 0.0f);
    
    // Bind cursor VAO and render
    glBindVertexArray(cursor_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    glBindVertexArray(0);
}

void WebGLRenderer::resize(int width, int height) {
    screen_width_ = width;
    screen_height_ = height;
    
    // Calculate cell dimensions based on font
    cell_width_ = font_metrics_.height * 0.6f; // Approximate monospace ratio
    cell_height_ = font_metrics_.height;
    
    set_viewport(0, 0, width, height);
}

void WebGLRenderer::set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
    
    // Update projection matrix
    float projection_matrix[16];
    build_orthographic_matrix(projection_matrix, 0.0f, width, height, 0.0f, -1.0f, 1.0f);
    
    glUseProgram(program_);
    glUniformMatrix4fv(u_projection_matrix_, 1, GL_FALSE, projection_matrix);
    
    glUseProgram(bg_program_);
    glUniformMatrix4fv(glGetUniformLocation(bg_program_, "u_projection_matrix"), 
                       1, GL_FALSE, projection_matrix);
    
    glUseProgram(cursor_program_);
    glUniformMatrix4fv(glGetUniformLocation(cursor_program_, "u_projection_matrix"), 
                       1, GL_FALSE, projection_matrix);
}

void WebGLRenderer::load_font(const std::string& font_family, int font_size, bool bold, bool italic) {
    // In a real implementation, this would load a font file
    // For now, we'll just update metrics
    
    font_metrics_.ascent = font_size * 0.75f;
    font_metrics_.descent = font_size * 0.25f;
    font_metrics_.height = font_size;
    font_metrics_.line_gap = 0;
    
    cell_width_ = font_size * 0.6f;
    cell_height_ = font_size;
    
    // Update glyph atlas
    update_glyph_atlas();
}

void WebGLRenderer::unload_font() {
    // Clean up font resources
    glyph_cache_.clear();
}

void WebGLRenderer::update_font_metrics() {
    // Update metrics based on current font
    load_font(terminal_->options().font_family, terminal_->options().font_size);
}

void WebGLRenderer::create_glyph_atlas() {
    // Create glyph atlas texture
    glGenTextures(1, &glyph_atlas_);
    glBindTexture(GL_TEXTURE_2D, glyph_atlas_);
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    // Initialize with empty data
    std::vector<uint8_t> empty_data(options_.atlas_size * options_.atlas_size * 4, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, options_.atlas_size, options_.atlas_size, 
                 0, GL_RGBA, GL_UNSIGNED_BYTE, empty_data.data());
    
    glyph_atlas_data_.resize(options_.atlas_size * options_.atlas_size * 4);
}

void WebGLRenderer::update_glyph_atlas() {
    // Update glyph atlas with common characters
    // In a real implementation, this would rasterize glyphs from a font
    
    // For now, we'll create a simple ASCII atlas
    const char* ascii_chars = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    
    for (const char* p = ascii_chars; *p; ++p) {
        uint32_t glyph_id = get_glyph_id(*p, false, false);
        if (glyph_cache_.find(glyph_id) == glyph_cache_.end()) {
            // Add glyph to atlas
            GlyphAtlasEntry entry;
            entry.glyph_id = glyph_id;
            entry.x = atlas_cursor_x_;
            entry.y = atlas_cursor_y_;
            entry.width = cell_width_;
            entry.height = cell_height_;
            entry.bearing_x = 0;
            entry.bearing_y = font_metrics_.ascent;
            entry.advance = cell_width_;
            entry.is_color = false;
            
            glyph_cache_[glyph_id] = entry;
            
            // Update cursor position
            atlas_cursor_x_ += cell_width_ + options_.glyph_padding;
            if (atlas_cursor_x_ + cell_width_ > options_.atlas_size) {
                atlas_cursor_x_ = 0;
                atlas_cursor_y_ += cell_height_ + options_.glyph_padding;
            }
        }
    }
}

void WebGLRenderer::create_textures() {
    // Create cell texture for background colors
    glGenTextures(1, &cell_texture_);
    glBindTexture(GL_TEXTURE_2D, cell_texture_);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    
    // Create background texture
    glGenTextures(1, &bg_texture_);
    glBindTexture(GL_TEXTURE_2D, bg_texture_);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    
    // Create cursor texture
    glGenTextures(1, &cursor_texture_);
    glBindTexture(GL_TEXTURE_2D, cursor_texture_);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

void WebGLRenderer::create_shaders() {
    // Compile vertex shader
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, get_vertex_shader_source());
    
    // Compile fragment shader
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, get_fragment_shader_source());
    
    // Link program
    program_ = link_program(vertex_shader, fragment_shader);
    
    // Get uniform locations
    u_projection_matrix_ = glGetUniformLocation(program_, "u_projection_matrix");
    u_cell_size_ = glGetUniformLocation(program_, "u_cell_size");
    u_glyph_atlas_ = glGetUniformLocation(program_, "u_glyph_atlas");
    u_cell_texture_ = glGetUniformLocation(program_, "u_cell_texture");
    
    // Create background shader program
    GLuint bg_vertex_shader = compile_shader(GL_VERTEX_SHADER, get_bg_vertex_shader_source());
    GLuint bg_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, get_bg_fragment_shader_source());
    bg_program_ = link_program(bg_vertex_shader, bg_fragment_shader);
    
    // Create cursor shader program
    GLuint cursor_vertex_shader = compile_shader(GL_VERTEX_SHADER, get_cursor_vertex_shader_source());
    GLuint cursor_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, get_cursor_fragment_shader_source());
    cursor_program_ = link_program(cursor_vertex_shader, cursor_fragment_shader);
    
    u_cursor_pos_ = glGetUniformLocation(cursor_program_, "u_cursor_pos");
    u_cursor_style_ = glGetUniformLocation(cursor_program_, "u_cursor_style");
    u_cursor_blink_ = glGetUniformLocation(cursor_program_, "u_blink_alpha");
    u_time_ = glGetUniformLocation(cursor_program_, "u_time");
}

void WebGLRenderer::create_buffers() {
    // Create VAOs
    glGenVertexArrays(1, &vao_);
    glGenVertexArrays(1, &bg_vao_);
    glGenVertexArrays(1, &cursor_vao_);
    
    // Create VBOs
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &bg_vbo_);
    glGenBuffers(1, &cursor_vbo_);
    
    // Create EBOs
    glGenBuffers(1, &ebo_);
    glGenBuffers(1, &bg_ebo_);
    glGenBuffers(1, &cursor_ebo_);
    
    // Setup vertex attributes for main program
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    
    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 10, (void*)0);
    glEnableVertexAttribArray(0);
    
    // Texture coordinate attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 10, (void*)(sizeof(float) * 2));
    glEnableVertexAttribArray(1);
    
    // Color attribute
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 10, (void*)(sizeof(float) * 4));
    glEnableVertexAttribArray(2);
    
    // Background color attribute
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 10, (void*)(sizeof(float) * 8));
    glEnableVertexAttribArray(3);
    
    glBindVertexArray(0);
}

void WebGLRenderer::clear() {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void WebGLRenderer::clear_line(int row) {
    // Clear a specific line
    // TODO: Implement line clearing
}

void WebGLRenderer::clear_range(int start_row, int start_col, int end_row, int end_col) {
    // Clear a range of cells
    // TODO: Implement range clearing
}

void WebGLRenderer::mark_dirty(int row) {
    if (row >= 0 && row < terminal_->rows()) {
        if (static_cast<int>(dirty_lines_.size()) <= row) {
            dirty_lines_.resize(terminal_->rows(), false);
        }
        dirty_lines_[row] = true;
    }
}

void WebGLRenderer::mark_all_dirty() {
    dirty_lines_.assign(terminal_->rows(), true);
    full_redraw_needed_ = true;
}

void WebGLRenderer::clear_dirty_flags() {
    dirty_lines_.assign(terminal_->rows(), false);
    full_redraw_needed_ = false;
}

bool WebGLRenderer::is_dirty(int row) const {
    if (row >= 0 && row < static_cast<int>(dirty_lines_.size())) {
        return dirty_lines_[row];
    }
    return false;
}

void WebGLRenderer::begin_frame() {
    // Setup frame state
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Update time uniform for cursor blink
    if (terminal_->options().cursor_blink) {
        float time = static_cast<float>(frame_count_ * 0.016f); // Approximate 60fps
        glUseProgram(cursor_program_);
        glUniform1f(u_time_, time);
    }
}

void WebGLRenderer::end_frame() {
    // Cleanup frame state
    glDisable(GL_BLEND);
}

void WebGLRenderer::flush() {
    // Flush any pending draws
    glFlush();
}

void WebGLRenderer::update_cell_texture(int row, int col, const Cell& cell) {
    // Update a single cell in the cell texture
    // TODO: Implement cell texture updates
}

void WebGLRenderer::update_background_texture() {
    // Update background texture with current terminal state
    // TODO: Implement background texture updates
}

void WebGLRenderer::update_cursor_texture() {
    // Update cursor texture
    // TODO: Implement cursor texture updates
}

void WebGLRenderer::upload_vertices() {
    // Upload vertex data to GPU
    // TODO: Implement vertex uploading
}

void WebGLRenderer::upload_indices() {
    // Upload index data to GPU
    // TODO: Implement index uploading
}

void WebGLRenderer::build_projection_matrix(float* matrix) {
    // Build orthographic projection matrix
    matrix[0] = 2.0f / screen_width_; matrix[1] = 0.0f; matrix[2] = 0.0f; matrix[3] = 0.0f;
    matrix[4] = 0.0f; matrix[5] = -2.0f / screen_height_; matrix[6] = 0.0f; matrix[7] = 0.0f;
    matrix[8] = 0.0f; matrix[9] = 0.0f; matrix[10] = -1.0f; matrix[11] = 0.0f;
    matrix[12] = -1.0f; matrix[13] = 1.0f; matrix[14] = 0.0f; matrix[15] = 1.0f;
}

void WebGLRenderer::build_orthographic_matrix(float* matrix, float left, float right, float bottom, float top, float near, float far) {
    matrix[0] = 2.0f / (right - left); matrix[1] = 0.0f; matrix[2] = 0.0f; matrix[3] = 0.0f;
    matrix[4] = 0.0f; matrix[5] = 2.0f / (top - bottom); matrix[6] = 0.0f; matrix[7] = 0.0f;
    matrix[8] = 0.0f; matrix[9] = 0.0f; matrix[10] = -2.0f / (far - near); matrix[11] = 0.0f;
    matrix[12] = -(right + left) / (right - left); matrix[13] = -(top + bottom) / (top - bottom); matrix[14] = -(far + near) / (far - near); matrix[15] = 1.0f;
}

void WebGLRenderer::render_cells() {
    glUseProgram(program_);
    
    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glyph_atlas_);
    glUniform1i(u_glyph_atlas_, 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, cell_texture_);
    glUniform1i(u_cell_texture_, 1);
    
    // Bind VAO
    glBindVertexArray(vao_);
    
    // Set cell size uniform
    glUniform2f(u_cell_size_, cell_width_, cell_height_);
    
    // Render each visible line
    for (int row = 0; row < terminal_->rows(); ++row) {
        if (is_dirty(row) || full_redraw_needed_) {
            render_line(row);
        }
    }
    
    glBindVertexArray(0);
}

void WebGLRenderer::render_background() {
    glUseProgram(bg_program_);
    
    // Bind background texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bg_texture_);
    
    // Bind VAO
    glBindVertexArray(bg_vao_);
    
    // Draw background quad
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    glBindVertexArray(0);
}

void WebGLRenderer::render_cursor_blink() {
    if (!terminal_->cursor().visible) return;
    
    render_cursor();
}

void WebGLRenderer::render_selection_range(const Selection& selection) {
    if (!selection.active) return;
    
    // Render selection highlight
    // TODO: Implement selection rendering
}

void WebGLRenderer::render_glyph(uint32_t glyph_id, int row, int col, const CellAttributes& attrs) {
    // Get glyph from atlas
    const GlyphAtlasEntry* entry = get_glyph_atlas_entry(glyph_id);
    if (!entry) return;
    
    // Calculate position
    float x = col * cell_width_;
    float y = row * cell_height_;
    
    // Build vertices for this glyph
    // In a real implementation, we'd batch these
    
    // For now, just render a colored rectangle
    // TODO: Implement actual glyph rendering
}

void WebGLRenderer::render_background_cell(int row, int col, uint32_t bg_color) {
    // Render background for a cell
    // TODO: Implement background cell rendering
}

GLuint WebGLRenderer::compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    // Check compilation status
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        
        if (info_len > 1) {
            std::vector<char> info_log(info_len);
            glGetShaderInfoLog(shader, info_len, nullptr, info_log.data());
            std::cerr << "Shader compilation failed: " << info_log.data() << std::endl;
        }
        
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

GLuint WebGLRenderer::link_program(GLuint vertex_shader, GLuint fragment_shader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    
    // Check link status
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);
        
        if (info_len > 1) {
            std::vector<char> info_log(info_len);
            glGetProgramInfoLog(program, info_len, nullptr, info_log.data());
            std::cerr << "Program linking failed: " << info_log.data() << std::endl;
        }
        
        glDeleteProgram(program);
        return 0;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return program;
}

uint32_t WebGLRenderer::get_glyph_id(char32_t codepoint, bool bold, bool italic) {
    // Simple glyph ID generation
    return (codepoint & 0xFFFFFF) | (bold ? 0x1000000 : 0) | (italic ? 0x2000000 : 0);
}

const GlyphAtlasEntry* WebGLRenderer::get_glyph_atlas_entry(uint32_t glyph_id) {
    auto it = glyph_cache_.find(glyph_id);
    return (it != glyph_cache_.end()) ? &it->second : nullptr;
}

void WebGLRenderer::screen_to_cell_coords(int screen_x, int screen_y, int& row, int& col) {
    col = screen_x / cell_width_;
    row = screen_y / cell_height_;
    
    // Clamp to valid range
    col = std::max(0, std::min(col, terminal_->cols() - 1));
    row = std::max(0, std::min(row, terminal_->rows() - 1));
}

void WebGLRenderer::cell_to_screen_coords(int row, int col, int& screen_x, int& screen_y) {
    screen_x = col * cell_width_;
    screen_y = row * cell_height_;
}

void WebGLRenderer::dump_state() const {
    std::cout << "WebGLRenderer State:" << std::endl;
    std::cout << "  Screen: " << screen_width_ << "x" << screen_height_ << std::endl;
    std::cout << "  Cell: " << cell_width_ << "x" << cell_height_ << std::endl;
    std::cout << "  Atlas size: " << options_.atlas_size << "x" << options_.atlas_size << std::endl;
    std::cout << "  Glyph cache size: " << glyph_cache_.size() << std::endl;
    std::cout << "  Frame count: " << frame_count_ << std::endl;
}

void WebGLRenderer::validate_state() const {
    // Validate OpenGL state
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::cerr << "OpenGL Error: " << error << std::endl;
    }
}

bool WebGLRenderer::validate_render_state() const {
    // Check if we have a valid OpenGL context
    // In a real implementation, we'd check for context validity
    return true;
}

bool WebGLRenderer::setup_gl_state() {
    // Setup OpenGL state
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    return true;
}

void WebGLRenderer::cleanup_gl_state() {
    // Clean up OpenGL resources
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    
    if (bg_program_) {
        glDeleteProgram(bg_program_);
        bg_program_ = 0;
    }
    
    if (cursor_program_) {
        glDeleteProgram(cursor_program_);
        cursor_program_ = 0;
    }
    
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    
    if (bg_vao_) {
        glDeleteVertexArrays(1, &bg_vao_);
        bg_vao_ = 0;
    }
    
    if (cursor_vao_) {
        glDeleteVertexArrays(1, &cursor_vao_);
        cursor_vao_ = 0;
    }
    
    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    
    if (bg_vbo_) {
        glDeleteBuffers(1, &bg_vbo_);
        bg_vbo_ = 0;
    }
    
    if (cursor_vbo_) {
        glDeleteBuffers(1, &cursor_vbo_);
        cursor_vbo_ = 0;
    }
    
    if (glyph_atlas_) {
        glDeleteTextures(1, &glyph_atlas_);
        glyph_atlas_ = 0;
    }
    
    if (cell_texture_) {
        glDeleteTextures(1, &cell_texture_);
        cell_texture_ = 0;
    }
    
    if (bg_texture_) {
        glDeleteTextures(1, &bg_texture_);
        bg_texture_ = 0;
    }
    
    if (cursor_texture_) {
        glDeleteTextures(1, &cursor_texture_);
        cursor_texture_ = 0;
    }
}

} // namespace spiritty