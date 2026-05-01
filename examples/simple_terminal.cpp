#include <spiritty/terminal.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace spiritty;

int main() {
    // Create terminal options
    TerminalOptions options;
    options.cols = 80;
    options.rows = 24;
    options.scrollback_lines = 1000;
    options.font_family = "monospace";
    options.font_size = 14;
    options.cursor_blink = true;
    options.word_wrap = true;
    options.gpu_acceleration = false; // Native builds don't have a WebGL context
    
    // Create terminal
    Terminal terminal(options);
    
    // Set up event handlers
    terminal.on_event("data", [](const TerminalEvent& event) {
        std::cout << "Terminal data: " << event.data << std::endl;
    });
    
    terminal.on_event("resize", [](const TerminalEvent& event) {
        std::cout << "Terminal resized to " << event.cols << "x" << event.rows << std::endl;
    });
    
    // Open terminal (in a web app, this would bind to a DOM element)
    terminal.open("terminal-container");
    
    // Write some test data
    terminal.write("Welcome to Spiritty!\r\n");
    terminal.write("This is a high-performance GPU-accelerated terminal.\r\n\r\n");
    
    // Test ANSI colors
    terminal.write("\x1B[31mRed text\x1B[0m\r\n");
    terminal.write("\x1B[32mGreen text\x1B[0m\r\n");
    terminal.write("\x1B[34mBlue text\x1B[0m\r\n");
    terminal.write("\x1B[1mBold text\x1B[0m\r\n");
    terminal.write("\x1B[3mItalic text\x1B[0m\r\n");
    terminal.write("\x1B[4mUnderlined text\x1B[0m\r\n");
    
    // Test 24-bit true color
    terminal.write("\x1B[38;2;255;100;100mRGB Red\x1B[0m\r\n");
    terminal.write("\x1B[48;2;100;255;100mRGB Green Background\x1B[0m\r\n");
    
    // Simulate some terminal output
    for (int i = 0; i < 10; ++i) {
        terminal.write("Line " + std::to_string(i) + "\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Test cursor movement
    terminal.write("\x1B[5;10HCursor moved to row 5, col 10\r\n");
    
    // Test clear screen
    terminal.write("Clearing screen in 3 seconds...\r\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    terminal.write("\x1B[2J");
    
    terminal.write("Screen cleared!\r\n");
    terminal.write("Type 'exit' to quit.\r\n");
    
    // Simple input loop
    std::string input;
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);
        
        if (input == "exit") {
            break;
        } else if (input == "clear") {
            terminal.clear();
        } else if (input == "reset") {
            terminal.reset();
        } else if (input == "resize") {
            terminal.resize(100, 30);
        } else {
            terminal.write("You typed: " + input + "\r\n");
        }
    }
    
    // Clean up
    terminal.destroy();
    
    return 0;
}