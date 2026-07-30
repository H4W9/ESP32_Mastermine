//                            USER DEFINED SETTINGS
//   TFT_eSPI setup for Marauder Pancake (ESP32-C5-DevKitC-1 + ST7796 3.5")
//
//   Bundled here so the CI build is self-contained: the workflow copies this into
//   the checked-out TFT_eSPI-ESP32-C5 library and points User_Setup_Select.h at it.
//   For a local Arduino build the equivalent file already ships in the installed
//   TFT_eSPI-ESP32-C5 library (see README).

// ##################################################################################
// Section 1. Driver
// ##################################################################################

//#define ILI9341_DRIVER
#define ST7796_DRIVER

// 3.5-inch portrait panel native resolution
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// If colours are inverted (white shows as black) then uncomment one of the next
// 2 lines try both options, one of the options should correct the inversion.

#define TFT_INVERSION_ON
// #define TFT_INVERSION_OFF

// ##################################################################################
// Section 2. Pin assignments (Pancake hardware)
// ##################################################################################

#define TFT_MISO  4
#define TFT_MOSI  24
#define TFT_SCLK  23
#define TFT_CS    5    // TFT chip select
#define TFT_DC    3    // Data/command
#define TFT_RST   2    // Reset
#define TFT_BL    26   // Backlight

#define TOUCH_CS  -1   // No resistive touch chip; FT6336 is I2C-only

// ##################################################################################
// Section 3. Fonts
// ##################################################################################

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
// #define LOAD_FONT6
// #define LOAD_FONT7
// #define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ##################################################################################
// Section 4. SPI speed
// ##################################################################################

// 80 MHz for a smooth cube: Mastermine pushes a full 320x404 sprite every frame
// while rotating, and at the old 27 MHz that push alone took ~77 ms, capping the
// frame rate near 13 fps (the "jerky rotation"). 80 MHz cuts it to ~26 ms (~39
// fps). The other H4W9 firmwares on this panel keep 27 MHz only because they
// never push a full sprite continuously, so it never limited them.
// If this shows pixel garbage or flicker on your panel, step down to 60000000
// or 40000000 — it is a signal-integrity limit of the wiring, not damage.
#define SPI_FREQUENCY       80000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
