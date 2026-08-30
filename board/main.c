#include <stdio.h>
#include <stdint.h> // Mandatory for hardware dev. We need exact bit-widths (uint32_t, uint8_t), standard 'int' is too risky.

// ============================================================================
// 1. Hardware Memory-Mapped I/O (MMIO)
// [CRITICAL WARNING] Every pointer to hardware registers MUST be 'volatile'!
// If you forget this, GCC running with -O2 optimization will assume you are
// reading a dead variable in a loop and optimize the read away. It will fail silently.
// ============================================================================

// RISC-V Machine Mode Timer (mtime)
#define MTIME_LOW       ((volatile uint32_t *)0xFF202100) // Lower 32 bits of the 64-bit system uptime counter
#define MTIME_HIGH      ((volatile uint32_t *)0xFF202104) // Upper 32 bits
#define MTIMECMP_LOW    ((volatile uint32_t *)0xFF202108) // Lower 32 bits of the comparator. Triggers interrupt when mtime >= mtimecmp
#define MTIMECMP_HIGH   ((volatile uint32_t *)0xFF20210C) // Upper 32 bits of the comparator

// Common Peripherals (Usually mapped via the Lightweight AXI/Avalon bridge)
#define LED_BASE        ((volatile uint32_t *) 0xFF200000) // 10 Red LEDs. Write 1 to turn on, 0 to turn off.
#define HEX3_HEX0       ((volatile uint32_t *) 0xFF200020) // Rightmost 4 7-segment displays (8 bits per display)
#define HEX5_HEX4       ((volatile uint32_t *) 0xFF200030) // Leftmost 2 7-segment displays
#define SWITCHES        ((volatile uint32_t *) 0xFF200040) // 10 Slide switches. Reads as 0x000 to 0x3FF
#define JTAG_UART_BASE  ((volatile uint32_t *) 0xFF201000) // UART interface. Lifesaver when you can't use printf.

// Pushbuttons & Interrupt Control
#define KEY_BASE        ((volatile uint32_t *) 0xFF200050) // Read current button levels
#define KEY_INT_MASK    ((volatile uint32_t *) 0xFF200058) // Interrupt mask. Write 1 to enable hardware interrupt for a specific button.
#define KEY_EDGE_CAP    ((volatile uint32_t *) 0xFF20005C) // Edge capture. Becomes 1 when pressed. MUST BE MANUALLY CLEARED.

// VGA Framebuffer Configuration
#define VGA_BASE        0x08000000 // Base address of the VGA pixel buffer (SRAM or SDRAM)
#define VGA_WIDTH       320        // Physical screen width in pixels
#define VGA_HEIGHT      240        // Physical screen height in pixels

// RGB565 Color Macros (16-bit color space: 5 bits Red, 6 bits Green, 5 bits Blue)
// How to calculate? E.g., Pure Red is 0xF800 (Binary: 11111 000000 00000)
#define COLOR_BLACK     0x0000
#define COLOR_GRAY      0x4208
#define COLOR_YELLOW    0xFFE0 // Max Red + Max Green = Yellow
#define COLOR_WHITE     0xFFFF  
#define COLOR_GREEN     0x07E0
#define COLOR_RED       0xF800
#define COLOR_DARKRED   0x4000 // Lowered the red bits for the flash crash blood-red background
#define COLOR_CYAN      0x07FF  
#define COLOR_PURPLE    0xF81F

// ============================================================================
// 2. Trading Engine Data Structures
// There is no malloc() or garbage collector in baremetal. Every byte must be 
// statically allocated to avoid stack overflows and memory corruption.
// ============================================================================
#define INITIAL_CAPITAL 500000  
#define MAX_PRICE_LEVELS 3      // Only rendering 3 levels up/down to save VRAM and CPU cycles
#define MAX_ORDERS_PER_LVL 10   // Hard-capped queue depth.

// Single Limit Order Node
typedef struct {
    int order_id;           // Auto-increment tracking ID
    int qty;                // Remaining size
    int ghost_qty;          // Historical size kept in memory to render the fade-out "ghost" UI effect
    int is_mine;            // Boolean: 1 if owned by player, 0 if market noise (Player orders render yellow)
    int visual_fx;          // UI State Machine: 0=Normal, 1=Flash white on fill, 2=Empty (Ghost, waiting for cleanup)
} L3Order;

// Aggregated Price Level (One tick in the book)
typedef struct {
    int price;              // Price of this specific level
    int total_qty;          // Cached sum of all order sizes. Saves CPU cycles (avoids looping every frame)
    int order_count;        // Length of the queue
    L3Order queue[MAX_ORDERS_PER_LVL]; // Static array acting as a FIFO queue
} L3PriceLevel;

// Top-level Order Book
typedef struct {
    L3PriceLevel bids[MAX_PRICE_LEVELS]; // Buyers (Bottom half)
    L3PriceLevel asks[MAX_PRICE_LEVELS]; // Sellers (Top half)
} L3OrderBook;

L3OrderBook ob;

// Color gradients to give the UI some depth (further away from mid-price = darker)
const uint16_t BID_COLORS[3] = {0x07E0, 0x05E0, 0x03E0}; // Green, Dark Green, Darker Green
const uint16_t ASK_COLORS[3] = {0xF800, 0xB000, 0x7800}; // Red, Dark Red, Darker Red

// Global Account State
long my_cash = INITIAL_CAPITAL;      
int my_inventory = 0;                // Current position (shares/contracts held)
long current_total_assets = INITIAL_CAPITAL; // Mark-to-market total value
int global_order_id = 1000;          

// ============================================================================
// 3. Multitasking & Interrupt Flags
// Variables shared between the ISR (Interrupt Service Routine) and the main() 
// loop MUST be declared 'volatile'. Otherwise, main() will cache them in a 
// register and ignore ISR updates.
// ============================================================================
volatile int tick_index = 0; // System absolute time scale
volatile int tick_flag = 0;  // Heartbeat flag: Timer sets to 1, main loop processes a frame, sets back to 0
int current_view_mode = 0;   // UI state: 0=Orderbook, 1=Dashboard

// Performance Counters
volatile int current_tps = 0;      // Actual Ticks Per Second
volatile int smooth_bar_width = 0; // Damped UI bar width to prevent flickering

// Peripheral Pipeline
long total_fill_volume = 0;      // Total traded volume (For 7-segment display)
uint32_t led_shift_reg = 0;      // Shift register for the LED ticker tape
int window_trade_qty = 0;        // Rolling window volume to trigger LED flashes

// Gamepad / Button Input Queues
volatile int is_paused = 0;        
volatile int player_buy_flag = 0;  
volatile int player_sell_flag = 0; 
volatile int player_panic_flag = 0;// Nuclear option (Cancel all orders)

// ============================================================================
// 4. Bitmap Fonts
// We don't have FreeType or vector graphics. Fonts are bit-masked by hand.
// A 16-bit integer represents a 3x5 pixel grid. We only use the lower 15 bits. 
// 1 = draw pixel, 0 = skip.
// ============================================================================
const uint16_t font3x5[10] = {
    0x7B6F, // 0 in binary: 0 111 101 101 101 111 (Imagine wrapping this into 3 cols, 5 rows)
    0x2C97, // 1
    0x73E7, // 2
    0x73CF, // 3
    0x5BC9, // 4
    0x79CF, // 5
    0x79EF, // 6
    0x7249, // 7
    0x7BEF, // 8
    0x7BCF  // 9
};

const uint16_t font3x5_letters[26] = {
    0x2BED, 0x6BAE, 0x3923, 0x6B6E, 0x79A7, 0x79A4, 0x396B, 0x5BED, 0x7497, 0x126B, 
    0x5D35, 0x4927, 0x5F6D, 0x6B6D, 0x2B6A, 0x6BA4, 0x2B59, 0x6BAD, 0x388E, 0x7492, 
    0x5B6B, 0x5B52, 0x5B7D, 0x5AAD, 0x5A92, 0x72A7
};

// ============================================================================
// 5. VGA Render Primitives
// Direct memory manipulation for graphics.
// ============================================================================

// Blit a solid rectangle directly to video RAM
void vga_draw_rect_fast(int x, int y, int w, int h, uint16_t color) {
    // [Hardware Protection] Bounds checking. Writing out of VGA bounds means 
    // overwriting random memory/registers. Instant crash or hard reset.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VGA_WIDTH) w = VGA_WIDTH - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;
    if (w <= 0 || h <= 0) return; 

    // VRAM Address Math: VGA_BASE + row_offset + col_offset
    // Why `(y << 10)`? The VGA IP core aligns row strides to 1024 pixels (2048 bytes) 
    // for easier hardware decoding. 1024 = 2^10.
    // Why `(x << 1)`? Each pixel is 16-bit (2 bytes), so multiply x by 2.
    for (int j = 0; j < h; j++) {
        volatile uint16_t *row_ptr = (volatile uint16_t *)(VGA_BASE + ((y + j) << 10) + (x << 1));
        for (int i = 0; i < w; i++) {
            *row_ptr++ = color; // Sequential pointer increment. Absolute fastest way to write memory in C.
        }
    }
}

// Construct a hollow box using 4 solid rectangles
void vga_draw_hollow_rect(int x, int y, int w, int h, uint16_t color) {
    vga_draw_rect_fast(x, y, w, 1, color);           // Top edge
    vga_draw_rect_fast(x, y + h - 1, w, 1, color);   // Bottom edge
    vga_draw_rect_fast(x, y, 1, h, color);           // Left edge
    vga_draw_rect_fast(x + w - 1, y, 1, h, color);   // Right edge
    // BUG FIX: original call was `vga_draw_rect_fast(x + w - 1, 1, h, color)`
    // — only 4 args against a 5-arg signature (x,y,w,h,color), and the y
    // coordinate was hardcoded to 1 instead of the box's actual y. That
    // would either fail to compile under a strict prototype, or (if the
    // compiler quietly matched args positionally) draw the right edge at
    // the wrong row using h as the color argument. Root cause: copy-paste
    // from the top-edge line without updating the argument list.
}

// Decode the bitmap table and render pixels
void vga_draw_digit(int x, int y, int digit, uint16_t color, int scale) {
    if (digit < 0 || digit > 9) return;
    uint16_t bitmap = font3x5[digit];
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            // Bitshift math to extract the specific pixel state from the 16-bit integer
            if (bitmap & (1 << (14 - (row * 3 + col)))) {
                vga_draw_rect_fast(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

// [Skipping redundant UI wrappers: vga_draw_letter, vga_draw_string, vga_draw_number]
// They are just wrappers around vga_draw_digit with character spacing (cx += 4 * scale)
void vga_draw_letter(int x, int y, char c, uint16_t color, int scale) {
    if (c < 'A' || c > 'Z') return;
    uint16_t bitmap = font3x5_letters[c - 'A'];
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (bitmap & (1 << (14 - (row * 3 + col)))) {
                vga_draw_rect_fast(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void vga_draw_string(int x, int y, const char *str, uint16_t color, int scale) {
    int cx = x;
    while (*str) {
        if (*str >= 'A' && *str <= 'Z') {
            vga_draw_letter(cx, y, *str, color, scale);
        } else if (*str >= '0' && *str <= '9') {
            vga_draw_digit(cx, y, *str - '0', color, scale);
        }
        cx += 4 * scale; 
        str++;
    }
}

void vga_draw_number(int x, int y, int num, uint16_t color, int scale) {
    if (num == 0) { 
        vga_draw_digit(x, y, 0, color, scale); 
        return; 
    }
    if (num < 0) { 
        vga_draw_rect_fast(x - 6, y + 2 * scale, 4, scale, color); 
        num = -num; 
    }
    int digits[12];
    int count = 0;
    int temp = num;
    while (temp > 0) { 
        digits[count++] = temp % 10; 
        temp /= 10; 
    }
    for (int i = 0; i < count; i++) {
        vga_draw_digit(x + (count - 1 - i) * 4 * scale, y, digits[i], color, scale);
    }
}

// Draw a number with a background block to prevent pixel smearing on update
void vga_draw_number_bg(int x, int y, int num, uint16_t color, int scale, int bg_width, uint16_t bg_color) {
    vga_draw_rect_fast(x - 8, y, (bg_width + 1) * 4 * scale, 5 * scale, bg_color);
    vga_draw_number(x, y, num, color, scale);
}

// Brute-force memory clear
void vga_clear_screen() {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        volatile uint16_t *row_ptr = (volatile uint16_t *)(VGA_BASE + (y << 10));
        for (int x = 0; x < VGA_WIDTH; x++) {
            *row_ptr++ = COLOR_BLACK;
        }
    }
}

void vga_draw_pause_icon(int show) {
    uint16_t color = show ? COLOR_WHITE : COLOR_BLACK;
    vga_draw_rect_fast(290, 20, 4, 15, color);
    vga_draw_rect_fast(298, 20, 4, 15, color);
}

// ============================================================================
// 6. UART Debug Drivers
// Polling-based UART. Keeps the system alive if printf fails.
// ============================================================================
void uart_putc(char c) {
    uint32_t control = *(JTAG_UART_BASE + 1);
    // Read the control register to check if the TX FIFO has free space
    if ((control & 0xFFFF0000) != 0) {
        *JTAG_UART_BASE = c; // Blast the char out
    }
}

void uart_print(const char *str) {
    while (*str) {
        uart_putc(*str++);
    }
}

// ============================================================================
// 7. Core Matching Engine (FIFO L3 Book)
// All running natively in on-chip SRAM.
// ============================================================================

void record_fill(int qty) {
    if (qty <= 0) return;
    total_fill_volume += qty;
    window_trade_qty += qty;
}

// Garbage Collector: Remove fully filled orders
void clean_ghosts(L3PriceLevel *lvl) {
    int w = 0; // Write pointer
    // Classic two-pointer in-place deletion. O(n) time, zero stack allocation.
    for(int r = 0; r < lvl->order_count; r++) {
        if(lvl->queue[r].visual_fx != 2 && lvl->queue[r].qty > 0) {
            lvl->queue[w++] = lvl->queue[r]; 
        }
    }
    lvl->order_count = w; // Shrink the queue length
}

void update_total_qty() {
    for(int i = 0; i < MAX_PRICE_LEVELS; i++) {
        int sb = 0, sa = 0;
        for(int q = 0; q < ob.bids[i].order_count; q++) {
            if(ob.bids[i].queue[q].visual_fx != 2) sb += ob.bids[i].queue[q].qty;
        }
        ob.bids[i].total_qty = sb;

        for(int q = 0; q < ob.asks[i].order_count; q++) {
            if(ob.asks[i].queue[q].visual_fx != 2) sa += ob.asks[i].queue[q].qty;
        }
        ob.asks[i].total_qty = sa;
    }
}

// Market aggressively dumping (Eating the bid side)
void simulate_market_sell(int qty) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob.bids[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob.bids[i].queue[q];
            if (ord->qty <= 0 || ord->visual_fx == 2) continue; // Skip dead/ghost orders
            
            // Execute order up to available size
            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty; 
            
            ord->qty -= fill; 
            qty -= fill; 
            
            // [Accounting] If it hit the player's order, settle the transaction
            if (ord->is_mine) { 
                my_inventory += fill;               // Player bought stock
                my_cash -= (fill * ob.bids[i].price); // Deduct cash
                record_fill(fill);                  
            }
            
            if (ord->qty == 0) { 
                ord->ghost_qty = prev; // Cache for the fade animation
                ord->visual_fx = 2;    // Tag for garbage collection
            }
        }
    }
}

// Market aggressively pumping (Eating the ask side)
void simulate_market_buy(int qty) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob.asks[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob.asks[i].queue[q];
            if (ord->qty <= 0 || ord->visual_fx == 2) continue;
            
            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty; 
            
            ord->qty -= fill; 
            qty -= fill;
            
            if (ord->is_mine) { 
                my_inventory -= fill; 
                my_cash += (fill * ob.asks[i].price); 
                record_fill(fill); 
            }
            
            if (ord->qty == 0) { 
                ord->ghost_qty = prev; 
                ord->visual_fx = 2; 
            }
        }
    }
}

// Player actively hitting the asks
void player_market_buy(int qty) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob.asks[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob.asks[i].queue[q];
            if (ord->qty <= 0 || ord->visual_fx == 2 || ord->is_mine) continue; // Don't eat our own orders
            
            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty; 
            
            ord->qty -= fill; 
            qty -= fill;
            
            my_inventory += fill; 
            my_cash -= (fill * ob.asks[i].price); 
            record_fill(fill);
            
            if (ord->qty == 0) { 
                ord->ghost_qty = prev; 
                ord->visual_fx = 2; 
            }
        }
    }
}

void player_market_sell(int qty) {
    for (int i = 0; i < MAX_PRICE_LEVELS && qty > 0; i++) {
        for (int q = 0; q < ob.bids[i].order_count && qty > 0; q++) {
            L3Order *ord = &ob.bids[i].queue[q];
            if (ord->qty <= 0 || ord->visual_fx == 2 || ord->is_mine) continue;
            
            int fill = (qty < ord->qty) ? qty : ord->qty;
            int prev = ord->qty; 
            
            ord->qty -= fill; 
            qty -= fill;
            
            my_inventory -= fill; 
            my_cash += (fill * ob.bids[i].price); 
            record_fill(fill);
            
            if (ord->qty == 0) { 
                ord->ghost_qty = prev; 
                ord->visual_fx = 2; 
            }
        }
    }
}

// Emergency: Cancel all active limit orders
void player_cancel_all_orders() {
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        // Cancel bids, refund cash
        for (int q = 0; q < ob.bids[i].order_count; q++) {
            L3Order *o = &ob.bids[i].queue[q];
            if (o->is_mine && o->qty > 0 && o->visual_fx != 2) { 
                my_cash += (o->qty * ob.bids[i].price); 
                o->visual_fx = 2; 
            }
        }
        // Cancel asks, refund inventory
        for (int q = 0; q < ob.asks[i].order_count; q++) {
            L3Order *o = &ob.asks[i].queue[q];
            if (o->is_mine && o->qty > 0 && o->visual_fx != 2) { 
                my_inventory += o->qty; 
                o->visual_fx = 2; 
            }
        }
    }
    update_total_qty();
}

// Seed initial liquidity
void init_l3_book(int base_price) {
    for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
        ob.bids[i].price = base_price - 1 - i;
        ob.bids[i].order_count = 3; 
        ob.bids[i].queue[0] = (L3Order){global_order_id++, 40, 0, 0, 0}; 
        ob.bids[i].queue[1] = (L3Order){global_order_id++, 80, 0, 0, 0}; 
        ob.bids[i].queue[2] = (L3Order){global_order_id++, 60, 0, 0, 0}; 

        ob.asks[i].price = base_price + 1 + i;
        ob.asks[i].order_count = 2; 
        ob.asks[i].queue[0] = (L3Order){global_order_id++, 50, 0, 0, 0}; 
        ob.asks[i].queue[1] = (L3Order){global_order_id++, 100, 0, 0, 0}; 
    }
    update_total_qty();
}

// ============================================================================
// 8. UI Rendering Engine
// ============================================================================

void vga_render_l3_micro(int is_crash_mode) {
    // Top Balance Bar
    int total_bids = 0, total_asks = 0;
    for(int i = 0; i < MAX_PRICE_LEVELS; i++) { 
        total_bids += ob.bids[i].total_qty; 
        total_asks += ob.asks[i].total_qty; 
    }
    
    if(total_bids > 0 || total_asks > 0) {
        int green_w = (total_bids * VGA_WIDTH) / (total_bids + total_asks);
        vga_draw_rect_fast(0, 0, green_w, 10, COLOR_GREEN); 
        if (VGA_WIDTH - green_w > 0) {
            vga_draw_rect_fast(green_w, 0, VGA_WIDTH - green_w, 10, COLOR_RED); 
        }
    }

    uint16_t axis_color = (ob.bids[0].total_qty <= 0 || ob.asks[0].total_qty <= 0) ? COLOR_CYAN : COLOR_GRAY;
    vga_draw_rect_fast(159, 10, 2, 210, axis_color); 

    vga_draw_rect_fast(220, 15, 90, 10, COLOR_BLACK);
    if (is_crash_mode && (tick_index % 20 < 10)) {
        vga_draw_string(220, 15, "CRASH TEST", COLOR_RED, 1);
    }

    // Render Asks
    for(int i = 0; i < MAX_PRICE_LEVELS; i++) {
        int y = 90 - i * 35; 
        int current_x = 162; 
        
        for(int q = 0; q < ob.asks[i].order_count; q++) {
            L3Order *ord = &ob.asks[i].queue[q];
            int draw_qty = (ord->visual_fx == 2) ? ord->ghost_qty : ord->qty;
            if(draw_qty <= 0) continue;
            
            int bar_w = draw_qty >> 1; 
            uint16_t color = (ord->visual_fx == 1) ? COLOR_WHITE : (ord->is_mine ? COLOR_YELLOW : ASK_COLORS[i]);
            
            if (ord->visual_fx == 1) ord->visual_fx = 0; 
            
            if (ord->visual_fx == 2) {
                vga_draw_hollow_rect(current_x, y, bar_w - 1, 25, COLOR_GRAY); 
            } else {
                vga_draw_rect_fast(current_x, y, bar_w - 1, 25, color); 
            }
            current_x += bar_w;
        }
        
        // Clean trailing artifacts
        if (current_x < VGA_WIDTH) vga_draw_rect_fast(current_x, y, VGA_WIDTH - current_x, 25, COLOR_BLACK);
        vga_draw_number_bg(270, y + 7, ob.asks[i].price, COLOR_WHITE, 2, 4, COLOR_BLACK);
    }
    
    // Render Bids
    for(int i = 0; i < MAX_PRICE_LEVELS; i++) {
        int y = 130 + i * 35; 
        int current_x = 158; 
        
        for(int q = 0; q < ob.bids[i].order_count; q++) {
            L3Order *ord = &ob.bids[i].queue[q];
            int draw_qty = (ord->visual_fx == 2) ? ord->ghost_qty : ord->qty;
            if(draw_qty <= 0) continue;
            
            int bar_w = draw_qty >> 1; 
            uint16_t color = (ord->visual_fx == 1) ? COLOR_WHITE : (ord->is_mine ? COLOR_YELLOW : BID_COLORS[i]);
            
            if (ord->visual_fx == 1) ord->visual_fx = 0;
            
            if (ord->visual_fx == 2) {
                vga_draw_hollow_rect(current_x - bar_w + 1, y, bar_w - 1, 25, COLOR_GRAY);
            } else {
                vga_draw_rect_fast(current_x - bar_w + 1, y, bar_w - 1, 25, color);
            }
            current_x -= bar_w;
        }
        
        if (current_x > 0) vga_draw_rect_fast(0, y, current_x, 25, COLOR_BLACK);
        vga_draw_number_bg(10, y + 7, ob.bids[i].price, COLOR_WHITE, 2, 4, COLOR_BLACK);
    }
}

void vga_render_balance_dashboard(int is_crash_mode) {
    long pnl = current_total_assets - INITIAL_CAPITAL;
    int scale = 4;       
    int text_scale = 3;  
    uint16_t bg_color = is_crash_mode ? COLOR_DARKRED : COLOR_BLACK;

    vga_draw_rect_fast(0, 0, VGA_WIDTH, VGA_HEIGHT, bg_color);

    vga_draw_rect_fast(10, 50, 300, 2, COLOR_GRAY);
    vga_draw_rect_fast(10, 110, 300, 2, COLOR_GRAY);
    vga_draw_rect_fast(10, 170, 300, 2, COLOR_GRAY);

    vga_draw_rect_fast(10, 20, 10, 15, COLOR_WHITE);
    vga_draw_string(28, 20, "ASSET", COLOR_WHITE, text_scale);
    vga_draw_number_bg(110, 17, current_total_assets, COLOR_WHITE, scale, 8, bg_color);

    vga_draw_rect_fast(10, 80, 10, 15, COLOR_GREEN);
    vga_draw_string(28, 80, "CASH", COLOR_GREEN, text_scale);
    vga_draw_number_bg(110, 77, my_cash, COLOR_GREEN, scale, 8, bg_color);

    vga_draw_rect_fast(10, 140, 10, 15, COLOR_YELLOW);
    vga_draw_string(28, 140, "POS", COLOR_YELLOW, text_scale);
    vga_draw_number_bg(110, 137, my_inventory, COLOR_YELLOW, scale, 6, bg_color); 

    uint16_t pnl_color = (pnl >= 0) ? COLOR_CYAN : COLOR_RED;
    vga_draw_rect_fast(10, 200, 10, 15, pnl_color);
    vga_draw_string(28, 200, "PNL", pnl_color, text_scale);
    
    int abs_pnl = pnl;
    if (pnl < 0) {
        vga_draw_rect_fast(95, 208, 10, 4, pnl_color); 
        abs_pnl = -pnl;
    } 
    vga_draw_number_bg(110, 197, abs_pnl, pnl_color, scale, 8, bg_color);

    if (is_crash_mode && (tick_index % 20 < 10)) {
        vga_draw_string(220, 15, "CRASH", COLOR_RED, 1);
    }
}

// Fake an exponential curve using segmented linear math (No float math allowed!)
int calc_bar_width_from_tps(int tps) {
    if (tps <= 0) return 0;
    if (tps >= 10000) return 300; 
    
    if (tps <= 10) return (tps * 40) / 10;                     
    if (tps <= 50) return 40 + ((tps - 10) * 60) / 40;         
    if (tps <= 200) return 100 + ((tps - 50) * 80) / 150;      
    if (tps <= 1000) return 180 + ((tps - 200) * 80) / 800;    
    return 260 + ((tps - 1000) * 40) / 9000;                   
}

uint32_t get_current_interval() {
    uint32_t sw = *SWITCHES;
    if (sw & 0x200) return 1000; 

    uint32_t speed_level = (sw >> 6) & 0x7; 
    switch(speed_level) {
        case 0: return 50000000; 
        case 1: return 25000000; 
        case 2: return 10000000; 
        case 3: return  2500000; 
        case 4: return   625000; 
        case 5: return   200000; 
        case 6: return   100000; 
        case 7: return    50000; 
        default: return 50000000;
    }
}

// ============================================================================
// 9. Core Simulation State Machine (Tick Processor)
// ============================================================================
void process_tick() {
    uint32_t sw = *SWITCHES;
    int is_crash_mode = (sw & 0x10); 
    
    if (tick_index == 0) init_l3_book(100);

    for(int i=0; i<MAX_PRICE_LEVELS; i++) { 
        clean_ghosts(&ob.bids[i]); 
        clean_ghosts(&ob.asks[i]); 
    }

    // Phase 1: Market Maker 
    if (tick_index % 2 == 0) {
        for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
            if (is_crash_mode) {
                // Flash Crash Isolation: Bids are unplugged. Asks aggressively stacked.
                if (ob.asks[i].order_count < MAX_ORDERS_PER_LVL) {
                    ob.asks[i].queue[ob.asks[i].order_count++] = (L3Order){global_order_id++, 800, 0, 0, 1};
                }
            } else {
                if (ob.bids[i].order_count < MAX_ORDERS_PER_LVL) {
                    int add_qty = (tick_index * 13 + i * 7) % 40 + 10;
                    ob.bids[i].queue[ob.bids[i].order_count++] = (L3Order){global_order_id++, add_qty, 0, 0, 1};
                }
                if (ob.asks[i].order_count < MAX_ORDERS_PER_LVL) {
                    int add_qty = (tick_index * 17 + i * 11) % 40 + 10;
                    ob.asks[i].queue[ob.asks[i].order_count++] = (L3Order){global_order_id++, add_qty, 0, 0, 1};
                }
            }
        }
    }

    // Phase 2: Event Injection
    if (is_crash_mode) {
        // Whale Dump Event
        if (tick_index % 80 == 0) {
            simulate_market_sell(20000); 
            for (int i = 0; i < MAX_PRICE_LEVELS; i++) {
                ob.bids[i].price -= 25; 
                ob.asks[i].price -= 25;
                if (ob.bids[i].price < 1) { 
                    ob.bids[i].price = 1; 
                    ob.asks[i].price = 2; 
                }
            }
            if (!(sw & 0x200)) uart_print("\n[!!!] WHALE DUMP DETECTED [!!!]\n");
        }
        
        // Dead Cat Bounce 
        if (ob.bids[0].price <= 5 && (tick_index % 120 == 50)) {
            for (int i = 0; i < MAX_PRICE_LEVELS; i++) { 
                ob.bids[i].price += 30; 
                ob.asks[i].price += 30; 
            }
        }
    } else {
        int vol = 40;
        if ((tick_index * 11) % 100 < 45) simulate_market_sell(vol); 
        else simulate_market_buy(vol);
    }

    update_total_qty();

    // Phase 3: Drift & Mean Reversion Physics
    if (tick_index % 3 == 0) {
        int drift = 0;
        
        if (is_crash_mode) {
            drift = -2; // Raw gravity, no reversion
        } else {
            int random_walk = (((tick_index * 17) % 11) - 5);
            int pull_force = (100 - (ob.bids[0].price + ob.asks[0].price) / 2) / 2;
            drift = random_walk + pull_force;
        }

        if (ob.bids[MAX_PRICE_LEVELS-1].price + drift > 1) {
            for (int i = 0; i < MAX_PRICE_LEVELS; i++) { 
                ob.bids[i].price += drift; 
                ob.asks[i].price += drift; 
            }
        }
    }

    current_total_assets = my_cash + (my_inventory * ob.bids[0].price);

    // Phase 4: UI Engine Rate Limiting
    int is_extreme = (sw & 0x200);
    int spd = (sw >> 6) & 0x7;
    int interval = is_extreme ? 1000 : (spd >= 7 ? 20 : 1); 
    
    if (tick_index % interval == 0) {
        if (current_view_mode == 0) {
            vga_render_l3_micro(is_crash_mode);
            if (is_crash_mode) vga_draw_hollow_rect(0, 0, 320, 240, COLOR_RED); 
        } else {
            vga_render_balance_dashboard(is_crash_mode);                     
        }
        
        int target = 0;
        if (current_tps > 0) {
            if (current_tps >= 10000) target = 300;
            else if (current_tps <= 10) target = (current_tps * 40) / 10;
            else if (current_tps <= 50) target = 40 + ((current_tps - 10) * 60) / 40;
            else if (current_tps <= 200) target = 100 + ((current_tps - 50) * 80) / 150;
            else if (current_tps <= 1000) target = 180 + ((current_tps - 200) * 80) / 800;
            else target = 260 + ((current_tps - 1000) * 40) / 9000;
        }

        if (target > 300) target = 300;
        
        if (is_extreme) smooth_bar_width = 300; 
        else smooth_bar_width += (target - smooth_bar_width) >> 2; // Basic low-pass filter
        if (smooth_bar_width < 0) smooth_bar_width = 0;
        
        vga_draw_rect_fast(10, 225, 300, 5, COLOR_GRAY);
        vga_draw_rect_fast(10, 225, smooth_bar_width, 5, (is_extreme ? COLOR_PURPLE : COLOR_CYAN));
        vga_draw_rect_fast(200, 215, 110, 8, COLOR_BLACK); 
        
        if (is_extreme) {
            vga_draw_string(200, 215, "TURBO", COLOR_PURPLE, 1);
        } else {
            vga_draw_string(200, 215, "TPS", (spd >= 6 ? COLOR_YELLOW : COLOR_CYAN), 1);
        }
        vga_draw_number_bg(230, 215, current_tps, (is_extreme ? COLOR_PURPLE : (spd >= 6 ? COLOR_YELLOW : COLOR_CYAN)), 1, 7, COLOR_BLACK);
    }
    tick_index++;
}

// ============================================================================
// 10. Peripherals & Interrupt Service Routine
// Directly interfacing with RISC-V Privileged Architecture
// ============================================================================

const uint8_t hex_segments[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x67};

void refresh_hex() {
    long v = (total_fill_volume < 0 ? 0 : total_fill_volume) % 1000000;
    uint32_t h1 = 0, h2 = 0;
    
    for(int i = 0; i < 4; i++) { 
        h1 |= (hex_segments[v % 10] << (i * 8)); 
        v /= 10; 
    }
    for(int i = 0; i < 2; i++) { 
        h2 |= (hex_segments[v % 10] << (i * 8)); 
        v /= 10; 
    }
    
    *HEX3_HEX0 = h1; 
    *HEX5_HEX4 = h2;
}

// [HARDWARE ISR] The attribute tells GCC to generate 'mret' instead of 'ret'
// and automatically save/restore caller-saved registers.
void __attribute__((interrupt("machine"))) isr(void) {
    uint32_t mcause; 
    // Read the Machine Cause Register to figure out who interrupted us
    asm volatile("csrr %0, mcause" : "=r"(mcause));
    
    // Exception Code 7 is the Machine Timer Interrupt
    if ((mcause & 0x7FFFFFFF) == 7) {
        uint32_t sw = *SWITCHES;
        uint32_t iv = (sw & 0x200) ? 1000 : (50000000 >> ((sw >> 6) & 0x7)); 
        
        uint64_t next = (((uint64_t)*MTIME_HIGH << 32) | *MTIME_LOW) + iv;
        
        // [CRITICAL] Set High to 0xFFFFFFFF first to prevent accidental triggers while writing Low
        *MTIMECMP_HIGH = 0xFFFFFFFF; 
        *MTIMECMP_LOW = (uint32_t)next; 
        *MTIMECMP_HIGH = (uint32_t)(next >> 32);
        
        tick_flag = 1; // Signal the main loop
    } 
    // Exception Code 18 is typically the External Interrupt line on Altera/Intel SoCs
    else if ((mcause & 0x7FFFFFFF) == 18) {
        uint32_t cap = *KEY_EDGE_CAP; 
        
        // Never put delays or heavy logic inside the ISR! Just update flags.
        if (cap & 0x1) is_paused = !is_paused;
        if (cap & 0x2) player_buy_flag = 1;
        if (cap & 0x4) player_sell_flag = 1;
        if (cap & 0x8) player_panic_flag = 1;
        
        // [ACK INTERRUPT] Write 1s to clear the edge capture bits. 
        // If you forget this, the ISR triggers infinitely as soon as you exit. Total lockup.
        *KEY_EDGE_CAP = 0xF; 
    }
}

// ============================================================================
// 11. Main Boot Sequence
// ============================================================================
int main() {
    vga_clear_screen();
    
    uart_print("===================================\n");
    uart_print("HFT Engine (Baremetal Test Build)\n");
    uart_print("===================================\n");
    
    refresh_hex();
    *LED_BASE = 0x0; 
    
    // Hardware Interrupt Setup
    *KEY_INT_MASK = 0xF;  
    *KEY_EDGE_CAP = 0xF; 

    // Point the Machine Trap-Vector Base-Address Register (mtvec) to our ISR
    asm volatile("csrw mtvec, %0" : : "r"(isr));
    
    // Enable Timer (bit 7) and External (bit 18) in the Machine Interrupt Enable (mie) register
    asm volatile("csrs mie, %0" : : "r"((1 << 18) | (1 << 7)));
    
    // Enable Global Interrupts via mstatus register
    asm volatile("li t0, 0x8 \n csrs mstatus, t0");
    
    // Kickstart the timer
    uint32_t sw = *SWITCHES;
    uint32_t iv = (sw & 0x200) ? 1000 : (50000000 >> ((sw >> 6) & 0x7));
    uint64_t next = (((uint64_t)*MTIME_HIGH << 32) | *MTIME_LOW) + iv;
    *MTIMECMP_HIGH = 0xFFFFFFFF; 
    *MTIMECMP_LOW = (uint32_t)next; 
    *MTIMECMP_HIGH = (uint32_t)(next >> 32);

    uint64_t last_calc_time = 0;
    uint64_t last_led_time = 0; 
    int last_tick_count = 0;

    // Background Architecture Loop
    while (1) {
        uint64_t now = (((uint64_t)*MTIME_HIGH << 32) | *MTIME_LOW);
        
        // Async Task 1: True TPS Calculation (250ms window)
        if (now - last_calc_time > 12500000) { 
            current_tps = (tick_index - last_tick_count) * 4; 
            last_calc_time = now; 
            last_tick_count = tick_index; 
        }
        
        // Async Task 2: LED Ticker Tape (50ms window)
        if (now - last_led_time > 2500000) {
            led_shift_reg = (led_shift_reg << 1) | (window_trade_qty > 0 ? 1 : 0);
            *LED_BASE = led_shift_reg & 0x3FF; 
            window_trade_qty = 0; 
            last_led_time = now;
        }

        // Async Task 3: Handle View Toggle
        if (((*SWITCHES) & 0x1) != current_view_mode) { 
            vga_clear_screen();
            current_view_mode = (*SWITCHES) & 0x1; 
        }

        // Sync Task: Consume the Timer Heartbeat
        if (tick_flag) {
            tick_flag = 0; // Acknowledge
            if (!is_paused) {
                vga_draw_pause_icon(0);
                process_tick();
                
                // Bot Logic (Evaluated synchronously with the tick)
                if ((*SWITCHES) & 0x20) {
                    int ask = ob.asks[0].price;
                    int is_crash_mode = ((*SWITCHES) & 0x10);
                    
                    int max_p = is_crash_mode ? 1000000 : 200;
                    
                    if (ask <= 98 && my_inventory < max_p) {
                        player_market_buy(250);
                    } else if (ob.bids[0].price >= 102 && my_inventory > 0) {
                        player_market_sell(250);
                    }
                }
            } else {
                vga_draw_pause_icon(1);
            }
        }
        
        // Async Task 4: Process Inputs (Kept out of the ISR for safety)
        if (player_buy_flag) { 
            player_buy_flag = 0; 
            player_market_buy(1000); 
        }
        if (player_sell_flag) { 
            player_sell_flag = 0; 
            player_market_sell(1000); 
        }
        if (player_panic_flag) { 
            player_panic_flag = 0; 
            player_cancel_all_orders(); 
        }
        
        // Hex displays run on a slow APB/Avalon bus. Don't write to them every frame.
        if (tick_index % 10 == 0) {
            refresh_hex();
        }
    }
}