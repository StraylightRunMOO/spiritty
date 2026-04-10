/**
 * Spiritty - High-performance GPU-accelerated terminal for the browser
 * Inspired by Ghostty's GPU rendering architecture
 */

(function(global) {
    'use strict';

    // Default options
    const DEFAULT_OPTIONS = {
        cols: 80,
        rows: 24,
        scrollbackLines: 10000,
        fontFamily: 'monospace',
        fontSize: 14,
        lineHeight: 1.2,
        cursorBlink: true,
        cursorStyle: 'block', // 'block', 'underline', 'bar'
        allowTransparency: false,
        theme: 'default',
        wordWrap: true,
        gpuAcceleration: true,
        antialias: true,
        colors: {
            foreground: '#ffffff',
            background: '#000000',
            cursor: '#ffffff',
            selection: 'rgba(128, 128, 128, 0.5)',
            palette: [
                '#000000', '#800000', '#008000', '#808000',
                '#000080', '#800080', '#008080', '#c0c0c0',
                '#808080', '#ff0000', '#00ff00', '#ffff00',
                '#0000ff', '#ff00ff', '#00ffff', '#ffffff'
            ]
        }
    };

    // ANSI color map
    const ANSI_COLORS = {
        0: 'black', 1: 'red', 2: 'green', 3: 'yellow',
        4: 'blue', 5: 'magenta', 6: 'cyan', 7: 'white',
        8: 'brightBlack', 9: 'brightRed', 10: 'brightGreen', 11: 'brightYellow',
        12: 'brightBlue', 13: 'brightMagenta', 14: 'brightCyan', 15: 'brightWhite'
    };

    /**
     * Spiritty Terminal Class
     */
    class SpirittyTerminal {
        constructor(container, options = {}) {
            this.container = typeof container === 'string' 
                ? document.getElementById(container) 
                : container;
            
            if (!this.container) {
                throw new Error('Spiritty: Container not found');
            }

            this.options = Object.assign({}, DEFAULT_OPTIONS, options);
            this.cols = this.options.cols;
            this.rows = this.options.rows;
            
            // Terminal state
            this.buffer = [];
            this.cursor = { row: 0, col: 0, visible: true, blinkState: true };
            this.selection = { active: false, startRow: 0, startCol: 0, endRow: 0, endCol: 0 };
            this.scrollTop = 0;
            this.modes = {
                insertMode: false,
                autoWrap: true,
                originMode: false,
                cursorVisible: true,
                cursorBlink: true,
                reverseVideo: false,
                newLineMode: false,
                mouseReporting: false,
                bracketedPaste: false,
                focusReporting: false
            };
            
            // Rendering state
            this.dirtyLines = new Set();
            this.needsFullRedraw = true;
            this.animationFrameId = null;
            
            // WebGL resources
            this.gl = null;
            this.program = null;
            this.bgProgram = null;
            this.cursorProgram = null;
            this.glyphAtlas = null;
            this.cellTexture = null;
            this.bgTexture = null;
            this.vao = null;
            this.vbo = null;
            this.ebo = null;
            
            // Font metrics
            this.fontMetrics = {
                ascent: 0,
                descent: 0,
                height: 0,
                lineGap: 0
            };
            
            // Glyph cache
            this.glyphCache = new Map();
            this.glyphAtlasData = null;
            this.atlasCursorX = 0;
            this.atlasCursorY = 0;
            this.atlasLineHeight = 0;
            
            // Event handlers
            this.eventHandlers = {
                data: [],
                resize: [],
                title: [],
                bell: [],
                cursorMove: [],
                scroll: [],
                selectionChange: []
            };
            
            // Initialize
            this.init();
        }

        init() {
            this.createContainer();
            this.createCanvas();
            this.initWebGL();
            this.createShaders();
            this.createBuffers();
            this.createTextures();
            this.createGlyphAtlas();
            this.measureFont();
            this.bindEvents();
            this.startRenderLoop();
            
            // Clear and initialize buffer
            this.clearBuffer();
            this.queueFullRedraw();
        }

        createContainer() {
            this.container.style.position = 'relative';
            this.container.style.overflow = 'hidden';
            this.container.style.cursor = 'text';
            this.container.style.userSelect = 'none';
            this.container.tabIndex = 0; // Make focusable
        }

        createCanvas() {
            this.canvas = document.createElement('canvas');
            this.canvas.style.position = 'absolute';
            this.canvas.style.top = '0';
            this.canvas.style.left = '0';
            this.canvas.style.width = '100%';
            this.canvas.style.height = '100%';
            
            this.container.appendChild(this.canvas);
            
            // Create overlay canvas for selection
            this.overlayCanvas = document.createElement('canvas');
            this.overlayCanvas.style.position = 'absolute';
            this.overlayCanvas.style.top = '0';
            this.overlayCanvas.style.left = '0';
            this.overlayCanvas.style.width = '100%';
            this.overlayCanvas.style.height = '100%';
            this.overlayCanvas.style.pointerEvents = 'none';
            
            this.container.appendChild(this.overlayCanvas);
        }

        initWebGL() {
            const gl = this.canvas.getContext('webgl2', {
                alpha: this.options.allowTransparency,
                antialias: this.options.antialias,
                preserveDrawingBuffer: false,
                powerPreference: 'high-performance'
            });
            
            if (!gl) {
                console.warn('Spiritty: WebGL2 not supported, falling back to canvas');
                this.initCanvas();
                return;
            }
            
            this.gl = gl;
            
            // Enable blending
            gl.enable(gl.BLEND);
            gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
        }

        initCanvas() {
            // Fallback to 2D canvas rendering
            this.ctx = this.canvas.getContext('2d');
            this.options.gpuAcceleration = false;
        }

        createShaders() {
            if (!this.gl) return;
            
            // Vertex shader source
            const vertexShaderSource = `
                #version 300 es
                
                layout(location = 0) in vec2 a_position;
                layout(location = 1) in vec2 a_texCoord;
                layout(location = 2) in vec4 a_color;
                layout(location = 3) in vec4 a_bgColor;
                
                uniform mat4 u_projection;
                uniform vec2 u_cellSize;
                
                out vec2 v_texCoord;
                out vec4 v_color;
                out vec4 v_bgColor;
                
                void main() {
                    vec2 pos = a_position * u_cellSize;
                    gl_Position = u_projection * vec4(pos, 0.0, 1.0);
                    v_texCoord = a_texCoord;
                    v_color = a_color;
                    v_bgColor = a_bgColor;
                }
            `;
            
            // Fragment shader source
            const fragmentShaderSource = `
                #version 300 es
                precision mediump float;
                
                in vec2 v_texCoord;
                in vec4 v_color;
                in vec4 v_bgColor;
                
                uniform sampler2D u_glyphAtlas;
                
                out vec4 fragColor;
                
                void main() {
                    float alpha = texture(u_glyphAtlas, v_texCoord).r;
                    vec4 textColor = vec4(v_color.rgb, v_color.a * alpha);
                    fragColor = mix(v_bgColor, textColor, textColor.a);
                }
            `;
            
            // Compile shaders
            const vertexShader = this.compileShader(gl.VERTEX_SHADER, vertexShaderSource);
            const fragmentShader = this.compileShader(gl.FRAGMENT_SHADER, fragmentShaderSource);
            
            // Link program
            this.program = this.linkProgram(vertexShader, fragmentShader);
            
            // Get uniform locations
            this.uniforms = {
                projection: this.gl.getUniformLocation(this.program, 'u_projection'),
                cellSize: this.gl.getUniformLocation(this.program, 'u_cellSize'),
                glyphAtlas: this.gl.getUniformLocation(this.program, 'u_glyphAtlas')
            };
            
            // Create background shader
            const bgVertexShaderSource = `
                #version 300 es
                
                layout(location = 0) in vec2 a_position;
                layout(location = 1) in vec4 a_color;
                
                uniform mat4 u_projection;
                
                out vec4 v_color;
                
                void main() {
                    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);
                    v_color = a_color;
                }
            `;
            
            const bgFragmentShaderSource = `
                #version 300 es
                precision mediump float;
                
                in vec4 v_color;
                
                out vec4 fragColor;
                
                void main() {
                    fragColor = v_color;
                }
            `;
            
            const bgVertexShader = this.compileShader(gl.VERTEX_SHADER, bgVertexShaderSource);
            const bgFragmentShader = this.compileShader(gl.FRAGMENT_SHADER, bgFragmentShaderSource);
            
            this.bgProgram = this.linkProgram(bgVertexShader, bgFragmentShader);
        }

        compileShader(type, source) {
            const shader = this.gl.createShader(type);
            this.gl.shaderSource(shader, source);
            this.gl.compileShader(shader);
            
            if (!this.gl.getShaderParameter(shader, this.gl.COMPILE_STATUS)) {
                console.error('Shader compilation failed:', this.gl.getShaderInfoLog(shader));
                this.gl.deleteShader(shader);
                return null;
            }
            
            return shader;
        }

        linkProgram(vertexShader, fragmentShader) {
            const program = this.gl.createProgram();
            this.gl.attachShader(program, vertexShader);
            this.gl.attachShader(program, fragmentShader);
            this.gl.linkProgram(program);
            
            if (!this.gl.getProgramParameter(program, this.gl.LINK_STATUS)) {
                console.error('Program linking failed:', this.gl.getProgramInfoLog(program));
                this.gl.deleteProgram(program);
                return null;
            }
            
            return program;
        }

        createBuffers() {
            if (!this.gl) return;
            
            // Create VAO
            this.vao = this.gl.createVertexArray();
            this.gl.bindVertexArray(this.vao);
            
            // Create VBO
            this.vbo = this.gl.createBuffer();
            this.gl.bindBuffer(this.gl.ARRAY_BUFFER, this.vbo);
            
            // Set up vertex attributes
            // Position (vec2)
            this.gl.vertexAttribPointer(0, 2, this.gl.FLOAT, false, 10 * 4, 0);
            this.gl.enableVertexAttribArray(0);
            
            // Texture coordinate (vec2)
            this.gl.vertexAttribPointer(1, 2, this.gl.FLOAT, false, 10 * 4, 2 * 4);
            this.gl.enableVertexAttribArray(1);
            
            // Color (vec4)
            this.gl.vertexAttribPointer(2, 4, this.gl.FLOAT, false, 10 * 4, 4 * 4);
            this.gl.enableVertexAttribArray(2);
            
            // Background color (vec4)
            this.gl.vertexAttribPointer(3, 4, this.gl.FLOAT, false, 10 * 4, 8 * 4);
            this.gl.enableVertexAttribArray(3);
            
            // Create EBO for indices
            this.ebo = this.gl.createBuffer();
            this.gl.bindBuffer(this.gl.ELEMENT_ARRAY_BUFFER, this.ebo);
            
            this.gl.bindVertexArray(null);
        }

        createTextures() {
            if (!this.gl) return;
            
            // Create glyph atlas texture
            this.glyphAtlas = this.gl.createTexture();
            this.gl.bindTexture(this.gl.TEXTURE_2D, this.glyphAtlas);
            
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_S, this.gl.CLAMP_TO_EDGE);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_T, this.gl.CLAMP_TO_EDGE);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MIN_FILTER, this.gl.LINEAR);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MAG_FILTER, this.gl.LINEAR);
            
            // Initialize with empty data
            const atlasSize = 1024;
            const emptyData = new Uint8Array(atlasSize * atlasSize * 4);
            this.gl.texImage2D(this.gl.TEXTURE_2D, 0, this.gl.RGBA, atlasSize, atlasSize, 
                               0, this.gl.RGBA, this.gl.UNSIGNED_BYTE, emptyData);
            
            this.glyphAtlasData = emptyData;
        }

        createGlyphAtlas() {
            // Create a simple ASCII atlas
            const chars = ' !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~';
            
            // In a real implementation, we'd rasterize glyphs from a font
            // For this example, we'll just create placeholder entries
            
            for (let i = 0; i < chars.length; i++) {
                const char = chars[i];
                const glyphId = this.getGlyphId(char.charCodeAt(0), false, false);
                
                this.glyphCache.set(glyphId, {
                    x: this.atlasCursorX,
                    y: this.atlasCursorY,
                    width: this.cellWidth,
                    height: this.cellHeight,
                    bearingX: 0,
                    bearingY: this.fontMetrics.ascent,
                    advance: this.cellWidth,
                    isColor: false
                });
                
                // Update cursor position
                this.atlasCursorX += this.cellWidth + 2;
                if (this.atlasCursorX + this.cellWidth > 1024) {
                    this.atlasCursorX = 0;
                    this.atlasCursorY += this.cellHeight + 2;
                }
            }
        }

        measureFont() {
            // Create a hidden canvas to measure font metrics
            const canvas = document.createElement('canvas');
            const ctx = canvas.getContext('2d');
            
            ctx.font = `${this.options.fontSize}px ${this.options.fontFamily}`;
            
            // Measure character size
            const metrics = ctx.measureText('M');
            this.cellWidth = metrics.width;
            this.cellHeight = this.options.fontSize * this.options.lineHeight;
            
            // Update font metrics
            this.fontMetrics.ascent = this.options.fontSize * 0.75;
            this.fontMetrics.descent = this.options.fontSize * 0.25;
            this.fontMetrics.height = this.options.fontSize;
            this.fontMetrics.lineGap = this.options.fontSize * (this.options.lineHeight - 1);
            
            // Resize canvas
            this.resizeCanvas();
        }

        resizeCanvas() {
            const rect = this.container.getBoundingClientRect();
            
            // Calculate dimensions based on cell size
            const width = this.cols * this.cellWidth;
            const height = this.rows * this.cellHeight;
            
            // Set canvas size
            this.canvas.width = width;
            this.canvas.height = height;
            this.canvas.style.width = rect.width + 'px';
            this.canvas.style.height = rect.height + 'px';
            
            // Set overlay canvas size
            this.overlayCanvas.width = width;
            this.overlayCanvas.height = height;
            this.overlayCanvas.style.width = rect.width + 'px';
            this.overlayCanvas.style.height = rect.height + 'px';
            
            // Update WebGL viewport
            if (this.gl) {
                this.gl.viewport(0, 0, width, height);
            }
            
            // Build projection matrix
            this.buildProjectionMatrix();
        }

        buildProjectionMatrix() {
            const width = this.cols * this.cellWidth;
            const height = this.rows * this.cellHeight;
            
            // Orthographic projection
            this.projectionMatrix = new Float32Array([
                2 / width, 0, 0, 0,
                0, -2 / height, 0, 0,
                0, 0, -1, 0,
                -1, 1, 0, 1
            ]);
        }

        bindEvents() {
            // Keyboard events
            this.container.addEventListener('keydown', (e) => {
                this.handleKeyDown(e);
            });
            
            this.container.addEventListener('keypress', (e) => {
                this.handleKeyPress(e);
            });
            
            // Mouse events
            this.container.addEventListener('mousedown', (e) => {
                this.handleMouseDown(e);
            });
            
            this.container.addEventListener('mousemove', (e) => {
                this.handleMouseMove(e);
            });
            
            this.container.addEventListener('mouseup', (e) => {
                this.handleMouseUp(e);
            });
            
            this.container.addEventListener('wheel', (e) => {
                this.handleWheel(e);
            });
            
            // Focus events
            this.container.addEventListener('focus', () => {
                this.focused = true;
                this.queueRender();
            });
            
            this.container.addEventListener('blur', () => {
                this.focused = false;
                this.queueRender();
            });
            
            // Context menu
            this.container.addEventListener('contextmenu', (e) => {
                e.preventDefault();
                this.handleContextMenu(e);
            });
            
            // Clipboard events
            this.container.addEventListener('copy', (e) => {
                this.handleCopy(e);
            });
            
            this.container.addEventListener('paste', (e) => {
                this.handlePaste(e);
            });
            
            // Resize events
            window.addEventListener('resize', () => {
                this.resizeCanvas();
                this.queueRender();
            });
        }

        handleKeyDown(e) {
            if (this.modes.bracketedPaste && e.key === 'v' && e.ctrlKey) {
                // Handle bracketed paste
                return;
            }
            
            let key = '';
            
            switch (e.key) {
                case 'Enter':
                    key = '\r';
                    break;
                case 'Backspace':
                    key = '\x7F';
                    break;
                case 'Tab':
                    key = '\t';
                    break;
                case 'Escape':
                    key = '\x1B';
                    break;
                case 'ArrowUp':
                    key = this.modes.cursorKeysMode ? '\x1BOA' : '\x1B[A';
                    break;
                case 'ArrowDown':
                    key = this.modes.cursorKeysMode ? '\x1BOB' : '\x1B[B';
                    break;
                case 'ArrowRight':
                    key = this.modes.cursorKeysMode ? '\x1BOC' : '\x1B[C';
                    break;
                case 'ArrowLeft':
                    key = this.modes.cursorKeysMode ? '\x1BOD' : '\x1B[D';
                    break;
                case 'Home':
                    key = '\x1B[H';
                    break;
                case 'End':
                    key = '\x1B[F';
                    break;
                case 'PageUp':
                    key = '\x1B[5~';
                    break;
                case 'PageDown':
                    key = '\x1B[6~';
                    break;
                case 'Delete':
                    key = '\x1B[3~';
                    break;
                case 'Insert':
                    key = '\x1B[2~';
                    break;
                default:
                    if (e.key.length === 1) {
                        key = e.key;
                    }
                    break;
            }
            
            if (key) {
                this.emit('data', key);
                e.preventDefault();
            }
        }

        handleKeyPress(e) {
            // Handle keypress for printable characters
            if (e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
                this.emit('data', e.key);
                e.preventDefault();
            }
        }

        handleMouseDown(e) {
            const rect = this.container.getBoundingClientRect();
            const row = Math.floor((e.clientY - rect.top) / (rect.height / this.rows));
            const col = Math.floor((e.clientX - rect.left) / (rect.width / this.cols));
            
            if (e.button === 0) { // Left button
                if (e.shiftKey && this.selection.active) {
                    // Extend selection
                    this.updateSelection(row, col);
                } else {
                    // Start new selection
                    const mode = e.detail === 2 ? 'word' : e.detail === 3 ? 'line' : 'normal';
                    this.startSelection(row, col, mode);
                }
            } else if (e.button === 2) { // Right button
                this.handleContextMenu(e);
            }
            
            e.preventDefault();
        }

        handleMouseMove(e) {
            if (this.selecting) {
                const rect = this.container.getBoundingClientRect();
                const row = Math.floor((e.clientY - rect.top) / (rect.height / this.rows));
                const col = Math.floor((e.clientX - rect.left) / (rect.width / this.cols));
                
                this.updateSelection(row, col);
            }
        }

        handleMouseUp(e) {
            if (this.selecting) {
                this.endSelection();
            }
        }

        handleWheel(e) {
            e.preventDefault();
            
            if (e.deltaY < 0) {
                this.scrollUp(3);
            } else {
                this.scrollDown(3);
            }
        }

        handleContextMenu(e) {
            // Show context menu with copy/paste options
            e.preventDefault();
        }

        handleCopy(e) {
            if (this.selection.active) {
                const text = this.getSelection();
                e.clipboardData.setData('text/plain', text);
                e.preventDefault();
            }
        }

        handlePaste(e) {
            const text = e.clipboardData.getData('text/plain');
            if (text) {
                this.emit('data', text);
                e.preventDefault();
            }
        }

        // Public API methods
        write(data) {
            // Parse ANSI escape sequences and update buffer
            this.parseData(data);
            this.queueRender();
        }

        resize(cols, rows) {
            this.cols = cols;
            this.rows = rows;
            this.resizeBuffer();
            this.resizeCanvas();
            this.queueRender();
            
            this.emit('resize', { cols, rows });
        }

        clear() {
            this.clearBuffer();
            this.cursor.row = 0;
            this.cursor.col = 0;
            this.queueFullRedraw();
        }

        reset() {
            this.clear();
            this.modes = Object.assign({}, DEFAULT_MODES);
            this.queueFullRedraw();
        }

        focus() {
            this.container.focus();
        }

        blur() {
            this.container.blur();
        }

        destroy() {
            if (this.animationFrameId) {
                cancelAnimationFrame(this.animationFrameId);
            }
            
            // Clean up WebGL resources
            if (this.gl) {
                this.gl.deleteProgram(this.program);
                this.gl.deleteProgram(this.bgProgram);
                this.gl.deleteTexture(this.glyphAtlas);
                this.gl.deleteBuffer(this.vbo);
                this.gl.deleteBuffer(this.ebo);
                this.gl.deleteVertexArray(this.vao);
            }
            
            // Remove from DOM
            if (this.container.parentNode) {
                this.container.removeChild(this.canvas);
                this.container.removeChild(this.overlayCanvas);
            }
        }

        getSelection() {
            if (!this.selection.active) return '';
            
            const normalized = this.normalizeSelection();
            let text = '';
            
            for (let row = normalized.startRow; row <= normalized.endRow; row++) {
                const line = this.buffer[row];
                const startCol = (row === normalized.startRow) ? normalized.startCol : 0;
                const endCol = (row === normalized.endRow) ? normalized.endCol : this.cols - 1;
                
                for (let col = startCol; col <= endCol; col++) {
                    const cell = line[col];
                    if (cell.codepoint && cell.codepoint !== ' ') {
                        text += String.fromCodePoint(cell.codepoint);
                    }
                }
                
                if (row < normalized.endRow) {
                    text += '\n';
                }
            }
            
            return text;
        }

        clearSelection() {
            this.selection.active = false;
            this.renderSelection();
            this.emit('selectionChange');
        }

        selectAll() {
            this.selection.active = true;
            this.selection.startRow = 0;
            this.selection.startCol = 0;
            this.selection.endRow = this.rows - 1;
            this.selection.endCol = this.cols - 1;
            
            this.renderSelection();
            this.emit('selectionChange');
        }

        scrollToTop() {
            this.scrollTop = 0;
            this.queueRender();
        }

        scrollToBottom() {
            this.scrollTop = Math.max(0, this.buffer.length - this.rows);
            this.queueRender();
        }

        scrollToLine(line) {
            this.scrollTop = Math.max(0, Math.min(line, this.buffer.length - this.rows));
            this.queueRender();
        }

        // Event handling
        on(event, handler) {
            if (!this.eventHandlers[event]) {
                this.eventHandlers[event] = [];
            }
            this.eventHandlers[event].push(handler);
        }

        off(event, handler) {
            if (!this.eventHandlers[event]) return;
            
            const index = this.eventHandlers[event].indexOf(handler);
            if (index !== -1) {
                this.eventHandlers[event].splice(index, 1);
            }
        }

        emit(event, data) {
            if (!this.eventHandlers[event]) return;
            
            this.eventHandlers[event].forEach(handler => {
                handler(data);
            });
        }

        // Selection methods
        startSelection(row, col, mode = 'normal') {
            this.selecting = true;
            this.selectionMode = mode;
            
            this.selectionStartRow = row;
            this.selectionStartCol = col;
            this.selectionEndRow = row;
            this.selectionEndCol = col;
            
            if (mode === 'word') {
                this.normalizeWordSelection();
            } else if (mode === 'line') {
                this.normalizeLineSelection();
            }
            
            this.renderSelection();
        }

        updateSelection(row, col) {
            if (!this.selecting) return;
            
            this.selectionEndRow = row;
            this.selectionEndCol = col;
            
            if (this.selectionMode === 'word') {
                this.normalizeWordSelection();
            } else if (this.selectionMode === 'line') {
                this.normalizeLineSelection();
            }
            
            this.renderSelection();
        }

        endSelection() {
            if (!this.selecting) return;
            
            this.selecting = false;
            this.selection.active = true;
            this.selection.startRow = this.selectionStartRow;
            this.selection.startCol = this.selectionStartCol;
            this.selection.endRow = this.selectionEndRow;
            this.selection.endCol = this.selectionEndCol;
            
            this.normalizeSelection();
            this.emit('selectionChange');
        }

        normalizeSelection() {
            const selection = Object.assign({}, this.selection);
            
            if (selection.startRow > selection.endRow ||
                (selection.startRow === selection.endRow && selection.startCol > selection.endCol)) {
                // Swap start and end
                [selection.startRow, selection.endRow] = [selection.endRow, selection.startRow];
                [selection.startCol, selection.endCol] = [selection.endCol, selection.startCol];
            }
            
            return selection;
        }

        normalizeWordSelection() {
            // Find word boundaries
            const boundaries = this.findWordBoundaries(this.selectionEndRow, this.selectionEndCol);
            this.selectionEndCol = boundaries.end;
            
            if (this.selectionStartRow === this.selectionEndRow && 
                this.selectionStartCol === this.selectionEndCol) {
                this.selectionStartCol = boundaries.start;
            }
        }

        normalizeLineSelection() {
            this.selectionStartCol = 0;
            this.selectionEndCol = this.cols - 1;
        }

        findWordBoundaries(row, col) {
            const line = this.buffer[row];
            
            // Skip non-word characters
            while (col < this.cols && !this.isWordCharacter(line[col].codepoint)) {
                col++;
            }
            
            if (col >= this.cols) {
                return { start: col, end: col };
            }
            
            let start = col;
            let end = col;
            
            // Find word start
            while (start > 0 && this.isWordCharacter(line[start - 1].codepoint)) {
                start--;
            }
            
            // Find word end
            while (end < this.cols && this.isWordCharacter(line[end].codepoint)) {
                end++;
            }
            
            return { start, end };
        }

        isWordCharacter(codepoint) {
            if (!codepoint) return false;
            const char = String.fromCodePoint(codepoint);
            return /[a-zA-Z0-9_]/.test(char);
        }

        renderSelection() {
            if (!this.selection.active && !this.selecting) {
                this.overlayCanvas.getContext('2d').clearRect(0, 0, 
                    this.overlayCanvas.width, this.overlayCanvas.height);
                return;
            }
            
            const selection = this.selecting ? {
                startRow: this.selectionStartRow,
                startCol: this.selectionStartCol,
                endRow: this.selectionEndRow,
                endCol: this.selectionEndCol
            } : this.selection;
            
            const normalized = this.normalizeSelection(selection);
            const ctx = this.overlayCanvas.getContext('2d');
            
            ctx.clearRect(0, 0, this.overlayCanvas.width, this.overlayCanvas.height);
            
            ctx.fillStyle = this.options.colors.selection;
            
            for (let row = normalized.startRow; row <= normalized.endRow; row++) {
                const startCol = (row === normalized.startRow) ? normalized.startCol : 0;
                const endCol = (row === normalized.endRow) ? normalized.endCol : this.cols - 1;
                
                const x = startCol * this.cellWidth;
                const y = row * this.cellHeight;
                const width = (endCol - startCol + 1) * this.cellWidth;
                const height = this.cellHeight;
                
                ctx.fillRect(x, y, width, height);
            }
        }

        // Render loop
        startRenderLoop() {
            const render = () => {
                if (this.needsRender) {
                    this.render();
                    this.needsRender = false;
                }
                
                // Update cursor blink
                this.updateCursorBlink();
                
                this.animationFrameId = requestAnimationFrame(render);
            };
            
            render();
        }

        updateCursorBlink() {
            if (!this.options.cursorBlink) return;
            
            const now = Date.now();
            if (now - this.lastCursorBlink > 500) {
                this.cursor.blinkState = !this.cursor.blinkState;
                this.lastCursorBlink = now;
                this.queueRender();
            }
        }

        render() {
            if (this.gl) {
                this.renderWebGL();
            } else {
                this.renderCanvas();
            }
        }

        renderWebGL() {
            this.gl.clearColor(0.0, 0.0, 0.0, 1.0);
            this.gl.clear(this.gl.COLOR_BUFFER_BIT);
            
            // Render background
            this.renderBackground();
            
            // Render cells
            this.renderCells();
            
            // Render cursor
            this.renderCursor();
        }

        renderBackground() {
            this.gl.useProgram(this.bgProgram);
            this.gl.uniformMatrix4fv(this.gl.getUniformLocation(this.bgProgram, 'u_projection'), 
                                    false, this.projectionMatrix);
            
            // Render background quads for each cell
            // In a real implementation, we'd batch these
        }

        renderCells() {
            this.gl.useProgram(this.program);
            this.gl.uniformMatrix4fv(this.uniforms.projection, false, this.projectionMatrix);
            this.gl.uniform2f(this.uniforms.cellSize, this.cellWidth, this.cellHeight);
            
            // Bind glyph atlas
            this.gl.activeTexture(this.gl.TEXTURE0);
            this.gl.bindTexture(this.gl.TEXTURE_2D, this.glyphAtlas);
            this.gl.uniform1i(this.uniforms.glyphAtlas, 0);
            
            // Render visible cells
            const startRow = this.scrollTop;
            const endRow = Math.min(startRow + this.rows, this.buffer.length);
            
            for (let row = startRow; row < endRow; row++) {
                const bufferRow = this.buffer[row];
                for (let col = 0; col < this.cols; col++) {
                    const cell = bufferRow[col];
                    if (cell.codepoint) {
                        this.renderCell(row - startRow, col, cell);
                    }
                }
            }
        }

        renderCell(row, col, cell) {
            // Build vertices for this cell
            const x = col * this.cellWidth;
            const y = row * this.cellHeight;
            
            // Get glyph from atlas
            const glyphId = this.getGlyphId(cell.codepoint, cell.attrs.bold, cell.attrs.italic);
            const glyph = this.glyphCache.get(glyphId);
            
            if (!glyph) return;
            
            // Calculate texture coordinates
            const atlasSize = 1024;
            const u1 = glyph.x / atlasSize;
            const v1 = glyph.y / atlasSize;
            const u2 = (glyph.x + glyph.width) / atlasSize;
            const v2 = (glyph.y + glyph.height) / atlasSize;
            
            // Build vertices
            const vertices = new Float32Array([
                // Position, texCoord, color, bgColor
                x, y, u1, v1, ...this.colorToFloat32(cell.attrs.fgColor), ...this.colorToFloat32(cell.attrs.bgColor),
                x + glyph.width, y, u2, v1, ...this.colorToFloat32(cell.attrs.fgColor), ...this.colorToFloat32(cell.attrs.bgColor),
                x + glyph.width, y + glyph.height, u2, v2, ...this.colorToFloat32(cell.attrs.fgColor), ...this.colorToFloat32(cell.attrs.bgColor),
                x, y + glyph.height, u1, v2, ...this.colorToFloat32(cell.attrs.fgColor), ...this.colorToFloat32(cell.attrs.bgColor)
            ]);
            
            // Upload and draw
            this.gl.bindBuffer(this.gl.ARRAY_BUFFER, this.vbo);
            this.gl.bufferData(this.gl.ARRAY_BUFFER, vertices, this.gl.DYNAMIC_DRAW);
            this.gl.drawArrays(this.gl.TRIANGLE_FAN, 0, 4);
        }

        renderCursor() {
            if (!this.cursor.visible || !this.cursor.blinkState) return;
            
            const x = this.cursor.col * this.cellWidth;
            const y = this.cursor.row * this.cellHeight;
            
            // In a real implementation, we'd use a shader to render the cursor
            // For this example, we'll just draw a rectangle
        }

        renderCanvas() {
            const ctx = this.ctx;
            
            // Clear canvas
            ctx.fillStyle = this.options.colors.background;
            ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
            
            // Set font
            ctx.font = `${this.options.fontSize}px ${this.options.fontFamily}`;
            ctx.textBaseline = 'top';
            
            // Render visible buffer
            const startRow = this.scrollTop;
            const endRow = Math.min(startRow + this.rows, this.buffer.length);
            
            for (let row = startRow; row < endRow; row++) {
                const bufferRow = this.buffer[row];
                for (let col = 0; col < this.cols; col++) {
                    const cell = bufferRow[col];
                    if (cell.codepoint) {
                        this.renderCellCanvas(row - startRow, col, cell, ctx);
                    }
                }
            }
            
            // Render cursor
            this.renderCursorCanvas(ctx);
        }

        renderCellCanvas(row, col, cell, ctx) {
            const x = col * this.cellWidth;
            const y = row * this.cellHeight;
            
            // Background
            if (cell.attrs.bgColor !== this.options.colors.background) {
                ctx.fillStyle = this.colorToCSS(cell.attrs.bgColor);
                ctx.fillRect(x, y, this.cellWidth, this.cellHeight);
            }
            
            // Text
            if (cell.codepoint && cell.codepoint !== ' ') {
                ctx.fillStyle = this.colorToCSS(cell.attrs.fgColor);
                
                if (cell.attrs.bold) {
                    ctx.font = `bold ${this.options.fontSize}px ${this.options.fontFamily}`;
                } else if (cell.attrs.italic) {
                    ctx.font = `italic ${this.options.fontSize}px ${this.options.fontFamily}`;
                } else {
                    ctx.font = `${this.options.fontSize}px ${this.options.fontFamily}`;
                }
                
                ctx.fillText(String.fromCodePoint(cell.codepoint), x, y);
            }
        }

        renderCursorCanvas(ctx) {
            if (!this.cursor.visible || !this.cursor.blinkState) return;
            
            const x = this.cursor.col * this.cellWidth;
            const y = this.cursor.row * this.cellHeight;
            
            ctx.fillStyle = this.options.colors.cursor;
            
            switch (this.options.cursorStyle) {
                case 'block':
                    ctx.fillRect(x, y, this.cellWidth, this.cellHeight);
                    break;
                case 'underline':
                    ctx.fillRect(x, y + this.cellHeight - 2, this.cellWidth, 2);
                    break;
                case 'bar':
                    ctx.fillRect(x, y, 2, this.cellHeight);
                    break;
            }
        }

        // Data parsing
        parseData(data) {
            // Simple parser for demonstration
            // In a real implementation, this would be a full ANSI parser
            
            for (let i = 0; i < data.length; i++) {
                const char = data[i];
                const code = char.charCodeAt(0);
                
                if (code === 27) { // ESC
                    // Handle escape sequences
                    i = this.parseEscapeSequence(data, i);
                } else if (code === 13) { // CR
                    this.cursor.col = 0;
                } else if (code === 10) { // LF
                    this.cursor.row++;
                    if (this.cursor.row >= this.rows) {
                        this.scrollUp();
                        this.cursor.row = this.rows - 1;
                    }
                } else if (code === 9) { // Tab
                    this.cursor.col = Math.min(this.cols - 1, this.cursor.col + 8 - (this.cursor.col % 8));
                } else if (code === 8) { // BS
                    this.cursor.col = Math.max(0, this.cursor.col - 1);
                } else if (code >= 32) { // Printable
                    this.writeCell(this.cursor.row, this.cursor.col, char);
                    this.cursor.col++;
                    if (this.cursor.col >= this.cols) {
                        this.cursor.col = 0;
                        this.cursor.row++;
                        if (this.cursor.row >= this.rows) {
                            this.scrollUp();
                            this.cursor.row = this.rows - 1;
                        }
                    }
                }
            }
        }

        parseEscapeSequence(data, start) {
            // Simple escape sequence parser
            // In a real implementation, this would handle all ANSI sequences
            
            if (start + 1 >= data.length) return start;
            
            const nextChar = data[start + 1];
            
            if (nextChar === '[') {
                // CSI sequence
                return this.parseCSISequence(data, start);
            } else if (nextChar === ']') {
                // OSC sequence
                return this.parseOSCSequence(data, start);
            }
            
            return start + 1;
        }

        parseCSISequence(data, start) {
            // Parse CSI (Control Sequence Introducer) sequences
            let i = start + 2;
            let params = '';
            
            while (i < data.length && data[i] !== 'm' && data[i] !== 'H' && data[i] !== 'J') {
                params += data[i];
                i++;
            }
            
            if (i < data.length) {
                const command = data[i];
                
                if (command === 'm') {
                    // SGR (Select Graphic Rendition)
                    this.parseSGR(params);
                } else if (command === 'H') {
                    // CUP (Cursor Position)
                    this.parseCUP(params);
                } else if (command === 'J') {
                    // ED (Erase in Display)
                    this.parseED(params);
                }
                
                return i;
            }
            
            return start + 1;
        }

        parseSGR(params) {
            // Parse Select Graphic Rendition parameters
            const paramList = params.split(';').map(p => parseInt(p) || 0);
            
            for (const param of paramList) {
                if (param === 0) {
                    // Reset
                    this.currentAttrs = this.getDefaultCellAttrs();
                } else if (param === 1) {
                    // Bold
                    this.currentAttrs.bold = true;
                } else if (param === 3) {
                    // Italic
                    this.currentAttrs.italic = true;
                } else if (param === 4) {
                    // Underline
                    this.currentAttrs.underline = true;
                } else if (param === 7) {
                    // Reverse
                    this.currentAttrs.reverse = true;
                } else if (param === 9) {
                    // Strikethrough
                    this.currentAttrs.strikethrough = true;
                } else if (param >= 30 && param <= 37) {
                    // Foreground colors
                    this.currentAttrs.fgColor = this.options.colors.palette[param - 30];
                } else if (param === 38) {
                    // Extended foreground color (24-bit)
                    // TODO: Parse 24-bit color
                } else if (param === 39) {
                    // Default foreground
                    this.currentAttrs.fgColor = this.options.colors.foreground;
                } else if (param >= 40 && param <= 47) {
                    // Background colors
                    this.currentAttrs.bgColor = this.options.colors.palette[param - 40];
                } else if (param === 48) {
                    // Extended background color (24-bit)
                    // TODO: Parse 24-bit color
                } else if (param === 49) {
                    // Default background
                    this.currentAttrs.bgColor = this.options.colors.background;
                }
            }
        }

        parseCUP(params) {
            // Parse Cursor Position
            const parts = params.split(';');
            const row = (parseInt(parts[0]) || 1) - 1;
            const col = (parseInt(parts[1]) || 1) - 1;
            
            this.cursor.row = Math.max(0, Math.min(row, this.rows - 1));
            this.cursor.col = Math.max(0, Math.min(col, this.cols - 1));
        }

        parseED(params) {
            // Parse Erase in Display
            const param = parseInt(params) || 0;
            
            switch (param) {
                case 0:
                    // Clear from cursor to end of screen
                    this.clearRange(this.cursor.row, this.cursor.col, this.rows - 1, this.cols - 1);
                    break;
                case 1:
                    // Clear from start of screen to cursor
                    this.clearRange(0, 0, this.cursor.row, this.cursor.col);
                    break;
                case 2:
                    // Clear entire screen
                    this.clear();
                    break;
                case 3:
                    // Clear scrollback
                    this.buffer = this.buffer.slice(-this.rows);
                    break;
            }
        }

        parseOSCSequence(data, start) {
            // Parse Operating System Command sequences
            // For window title, colors, etc.
            return start + 1;
        }

        // Buffer management
        clearBuffer() {
            this.buffer = [];
            for (let i = 0; i < this.rows; i++) {
                this.buffer.push(this.createLine());
            }
        }

        createLine() {
            const line = [];
            for (let i = 0; i < this.cols; i++) {
                line.push(this.createCell());
            }
            return line;
        }

        createCell() {
            return {
                codepoint: 0,
                width: 1,
                attrs: this.getDefaultCellAttrs()
            };
        }

        getDefaultCellAttrs() {
            return {
                fgColor: this.options.colors.foreground,
                bgColor: this.options.colors.background,
                bold: false,
                italic: false,
                underline: false,
                strikethrough: false,
                dim: false,
                reverse: false,
                hidden: false
            };
        }

        writeCell(row, col, char) {
            if (row < 0 || row >= this.rows || col < 0 || col >= this.cols) return;
            
            const line = this.buffer[row];
            const cell = line[col];
            
            cell.codepoint = char.codePointAt(0);
            cell.attrs = Object.assign({}, this.currentAttrs);
            
            this.markDirty(row);
        }

        clearRange(startRow, startCol, endRow, endCol) {
            for (let row = startRow; row <= endRow; row++) {
                const line = this.buffer[row];
                const start = (row === startRow) ? startCol : 0;
                const end = (row === endRow) ? endCol : this.cols - 1;
                
                for (let col = start; col <= end; col++) {
                    const cell = line[col];
                    cell.codepoint = 0;
                    cell.attrs = this.getDefaultCellAttrs();
                }
                
                this.markDirty(row);
            }
        }

        resizeBuffer() {
            // Resize buffer to new dimensions
            const newBuffer = [];
            
            for (let i = 0; i < this.rows; i++) {
                if (i < this.buffer.length) {
                    // Copy existing row
                    const row = this.buffer[i];
                    const newRow = [];
                    
                    for (let j = 0; j < this.cols; j++) {
                        if (j < row.length) {
                            newRow.push(Object.assign({}, row[j]));
                        } else {
                            newRow.push(this.createCell());
                        }
                    }
                    
                    newBuffer.push(newRow);
                } else {
                    // Create new row
                    newBuffer.push(this.createLine());
                }
            }
            
            this.buffer = newBuffer;
        }

        scrollUp(count = 1) {
            for (let i = 0; i < count; i++) {
                this.buffer.shift();
                this.buffer.push(this.createLine());
            }
            this.queueFullRedraw();
        }

        scrollDown(count = 1) {
            for (let i = 0; i < count; i++) {
                this.buffer.pop();
                this.buffer.unshift(this.createLine());
            }
            this.queueFullRedraw();
        }

        getGlyphId(codepoint, bold, italic) {
            return (codepoint & 0xFFFFFF) | (bold ? 0x1000000 : 0) | (italic ? 0x2000000 : 0);
        }

        colorToFloat32(color) {
            // Convert hex color to float32 array
            const r = ((color >> 16) & 0xFF) / 255;
            const g = ((color >> 8) & 0xFF) / 255;
            const b = (color & 0xFF) / 255;
            const a = ((color >> 24) & 0xFF) / 255;
            return [r, g, b, a];
        }

        colorToCSS(color) {
            // Convert hex color to CSS color
            const r = (color >> 16) & 0xFF;
            const g = (color >> 8) & 0xFF;
            const b = color & 0xFF;
            return `rgb(${r}, ${g}, ${b})`;
        }

        markDirty(row) {
            this.dirtyLines.add(row);
        }

        markAllDirty() {
            this.needsFullRedraw = true;
        }

        queueRender() {
            this.needsRender = true;
        }

        queueFullRedraw() {
            this.needsFullRedraw = true;
            this.queueRender();
        }
    }

    // Export Spiritty
    global.Spiritty = SpirittyTerminal;

    // Create Spiritty namespace
    global.spiritty = global.spiritty || {};
    global.spiritty.Terminal = SpirittyTerminal;

})(typeof window !== 'undefined' ? window : this);