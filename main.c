#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include <stdio.h>        // printf
#include <stdlib.h>       // malloc
#include <string.h>       // memcpy
#include "math.h"         // ceil() & log2() for Generalised Block
#include "pulsegen.pio.h" // PIO pulse generator

// Timing
#define ASM_OFFSET 3 // We "lose" 3 ticks in prep
#define FREQ 3500000 // ZX Spectrum 48k clock frequency
// #define FREQ 3540000 // ZX Spectrum 128K clock frequency

// User Customisations
// #define USE_FATFS 1 // Enable SD Card support
// #define USE_ZLIB 1 // Enable CSW Compression support
#define AUDIO_PIN 28 // Output GPIO (Olimex PICO PC - PWM Audio Left on GPIO 28)

// Tapes used for testing
#define FILENAME "DIZZY7.tzx"   // Block: Turbo Loader
// #define FILENAME "AAHKU.tzx"    // Block: Pure Tone (Speed Loader)
// #define FILENAME "FIRST.tzx"    // Block: RAW Data (Direct Recording)
// #define FILENAME "EXPLOSIO.tzx" // Block: CSW Data (Compressed Square Wave)
// #define FILENAME "YANKEE.tzx"   // Block: Generalised Data
// #define FILENAME "HOLPOKER.tzx" // Behaviour: Sequences / Jump / Groups
// #define FILENAME "CASIO.tzx"    // Behaviour: Level set
// #define FILENAME "LONEWOL3.tzx" // Behaviour: Menu / Signal-sensive
// #define FILENAME "ESKIMOCA.tzx" // Comments - Unused

/*
 * For SD Card Support, embed FatFs_SPI from and edit hw_config.c:
 * https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico
 */
#ifdef USE_FATFS
// Use SD Card
#include "f_util.h"
#include "ff.h"
#include "rtc.h"
#include "hw_config.h"
#else
// Use a basic, distributable example:
// #include "samples/tzx-basic.h" // TZX "Hello" in BASIC
#include "samples/tap-basic.h" // TAP "Hello" in BASIC
#endif

/*
 * For CSW compression, embed inflate.c / inflate.h from:
 * https://github.com/derf/zlib-deflate-nostdlib
 */
#ifdef USE_ZLIB
#include "inflate.h"
#endif

int gpio_level = 1; // we will start LOW (and be held that way on first pulse)

enum blocks
{
    BLK_STD = 0x10,
    BLK_TURBO,
    BLK_TONE,
    BLK_PULSES,
    BLK_PDATA,
    BLK_DIRECT,
    BLK_CSW = 0x18,
    BLK_GENERAL,
    BLK_PAUSE = 0x20,
    BLK_GROUP_START,
    BLK_GROUP_END,
    BLK_JUMP,
    BLK_LOOP_START,
    BLK_LOOP_END,
    BLK_SEQ_CALL,
    BLK_SEQ_RET,
    BLK_SEL,
    BLK_STOP_48K = 0x2A,
    BLK_SIG_LEVEL,
    BLK_TEXT = 0x30,
    BLK_MSG,
    BLK_INFO,
    BLK_HARDWARE,
    BLK_CUSTOM = 0x35,
    BLK_GLUE = 0x5A
};

// Contains all the possible data block metadata
typedef struct t_block_desc
{
    // Common data
    uint8_t type;
    uint16_t pause;

    // For standard blocks
    uint16_t p_pulse;
    uint16_t sync_a;
    uint16_t sync_b;
    uint16_t bit_zero;
    uint16_t bit_one;
    uint8_t used_bits;

    // For pulses
    uint32_t p_total;
    uint8_t p_max_pulses;
    uint8_t p_symbols;

    // For data
    uint32_t d_total;
    uint8_t d_max_pulses;
    uint8_t d_symbols;

    // CSW
    uint32_t sample_ticks;
    uint8_t compression;

    // Length (data or block)
    uint32_t blklen;
} t_block_desc;

#ifdef USE_FATFS
uint32_t get_tzx_from_sdcard(uint8_t **tape, char tzxfile[])
{
    FIL fh;
    FRESULT fres;
    time_init();

    // Mount SD card
    sd_card_t *pSD = sd_get_by_num(0);
    fres = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (FR_OK != fres)
        panic("f_mount error: %s (%d)\n", FRESULT_str(fres), fres);

    // Open file
    fres = f_open(&fh, tzxfile, FA_READ);
    if (FR_OK != fres && FR_EXIST != fres)
        panic("f_open(%s) error: %s (%d)\n", tzxfile, FRESULT_str(fres), fres);

    // Get file size and allocate memory
    uint32_t tapesize = f_size(&fh);
    printf("Found file '%s': %u bytes\n", tzxfile, tapesize);
    *tape = malloc(tapesize * sizeof(uint8_t));
    if (*tape == NULL)
    {
        printf("malloc error: cannot allocate memory\n");
    }

    // Read file into memory
    fres = f_read(&fh, *tape, tapesize, NULL);
    if (FR_OK != fres)
    {
        printf("f_read error: %s (%d)\n", FRESULT_str(fres), fres);
    }

    // Close file
    fres = f_close(&fh);
    if (FR_OK != fres)
    {
        printf("f_close error: %s (%d)\n", FRESULT_str(fres), fres);
    }

    // Unmount SD card
    f_unmount(pSD->pcName);

    return tapesize;
}
#endif

uint32_t parse_int(uint8_t buf[], int width)
{
    uint32_t value = 0;
    for (int x = 0; x < width; x++)
    {
        value |= (buf[x] << (x * 8));
    }
    return value;
}

uint32_t *validate_tape(uint8_t buf[], uint32_t file_len)
{
    /*
     * This validates the tape by making sure the length adds up
     * Also notes block offsets used by 0x23, 0x26, 0x28
     *
     * Not using this to store further metadata to save memory
     * Instead we can process the metadata in-situ
     */
    uint16_t i = 0;
    uint32_t *block_addr = malloc(1 * sizeof(uint32_t));
    uint32_t length, addr = 0;

    // Check if there's a TZX header: { "Z", "X", "T", }
    if ((buf[0] == 0x5A) && (buf[1] == 0x58) && (buf[2] == 0x54))
    {
        addr = 10; // Skip TZX header

        while (addr < file_len)
        {
            /*
             * Calculate the block length:
             * - Block Type byte +
             * - Block Attributes size +
             * - Block Data size (from attribute)
             */
            switch (buf[addr])
            {
            /*
             * DATA BLOCKS
             */
            case BLK_STD:
                length = 5 + parse_int(buf + addr + 0x03, 2);
                break;

            case BLK_TURBO:
                length = 19 + parse_int(buf + addr + 0x10, 3);
                break;

            case BLK_TONE:
                length = 5;
                break;

            case BLK_PULSES:
                length = 2 + parse_int(buf + addr + 0x01, 1) * 2;
                break;

            case BLK_PDATA:
                length = 11 + parse_int(buf + addr + 0x08, 3);
                break;

            case BLK_DIRECT:
                length = 9 + parse_int(buf + addr + 0x06, 3);
                break;

            case BLK_CSW:
                length = 5 + parse_int(buf + addr + 0x01, 4);
                break;

            case BLK_GENERAL:
                length = 5 + parse_int(buf + addr + 0x01, 4);
                break;

            /*
             * BEHAVIOUR BLOCKS
             */
            case BLK_PAUSE:
                length = 3;
                break;

            case BLK_GROUP_START:
                length = 2 + parse_int(buf + addr + 0x01, 1);
                break;

            case BLK_GROUP_END:
                length = 1;
                break;

            case BLK_JUMP:
                length = 3;
                break;

            case BLK_LOOP_START:
                length = 3;
                break;

            case BLK_LOOP_END:
                length = 1;
                break;

            case BLK_SEQ_CALL:
                // Array is word-based
                length = 3 + parse_int(buf + addr + 0x01, 2) * 2;
                break;

            case BLK_SEQ_RET:
                length = 1;
                break;

            case BLK_SEL:
                // Use length of BLOCK, not SELECTIONS
                length = 3 + parse_int(buf + addr + 0x01, 2);
                break;

            case BLK_STOP_48K:
                length = 5;
                break;

            case BLK_SIG_LEVEL:
                length = 6;
                break;

            /*
             * INFO BLOCKS
             */
            case BLK_TEXT:
                length = 2 + parse_int(buf + addr + 0x01, 1);
                break;

            case BLK_MSG:
                length = 3 + parse_int(buf + addr + 0x02, 1);
                break;

            case BLK_INFO:
                // Note: this is BLOCK length, not TEXT
                length = 3 + parse_int(buf + addr + 0x01, 2);
                break;

            case BLK_HARDWARE:
                // This is an array of 3 bytes
                length = 2 + parse_int(buf + addr + 0x01, 1) * 3;
                break;

            case BLK_CUSTOM:
                length = 21 + parse_int(buf + addr + 0x11, 4);
                break;

            /*
             * GLUE BLOCK (CONCATENATION)
             */
            case BLK_GLUE:
                length = 10;
                break;

            default:
                printf("%02x unknown!\n", buf[addr]);
                free(block_addr);
                return NULL;
            }
            // Block checks out, add it
            printf("%u: Type: 0x%02x, Bytes: %u\n", i, buf[addr], length);
            block_addr[i++] = addr;
            addr += length;

            // Add another space for next block
            block_addr = realloc(block_addr, (i + 1) * sizeof(uint32_t));
            block_addr[i] = UINT32_MAX;
        }
        if (addr == file_len)
        {
            printf("End of TZX %u of %u\n", addr, file_len);
            return block_addr;
        }
        else
        {
            printf("Unexpected End\n", addr, file_len);
            free(block_addr);
            return NULL;
        }
    }
    else
    {
        // Try to see if it's a TAP file - hope all the lengths add up!
        while (addr < file_len)
        {
            // Length word + data
            length = 2 + parse_int(buf + addr, 2);

            // Add block to list
            printf("%u: Bytes: %u\n", i, length);
            block_addr[i++] = addr;

            // add a new entry and fill it with a silly value
            block_addr = realloc(block_addr, (i + 1) * sizeof(uint32_t));
            block_addr[i] = UINT32_MAX;

            // Move to next part
            addr += length;
        }
        if (addr == file_len)
        {
            printf("End of TAP %u of %u\n", addr, file_len);
            return block_addr;
        }
        else
        {
            printf("Unexpected End\n", addr, file_len);
            free(block_addr);
            return NULL;
        }
    }

    printf("Not a valid tape file\n");
    free(block_addr);
    return NULL;
}

// Send the pulse of duration ticks
void send_pulse(PIO pio, int sm, uint16_t duration)
{
    if (duration > 0)
    {
        // Send pulse with assembler offset
        pio_sm_put_blocking(pio, sm, duration - ASM_OFFSET);
        // State tracker for signal levels
        gpio_level ^= 1;
    }
    else
    {
        // send nothing, means next pulse remains at this level
        pio_sm_put_blocking(pio, sm, 0);
    }
}

// Send a tone of fixed length over a number of pulses
void send_pure_tone_block(PIO pio, int sm, uint16_t pulses, uint16_t duration)
{
    for (uint16_t i = 0; i < pulses; i++)
    {
        send_pulse(pio, sm, duration);
    }
}

// Send a stream of pulses of set lengths - CSW and Pulse Sequence blocks
void send_pulse_array(PIO pio, int sm, uint32_t pulses, uint8_t lengths[])
{
    // Lengths are uint16_t
    for (uint32_t i = 0; i < pulses; i++)
    {
        send_pulse(pio, sm, lengths[i * 2] + (lengths[i * 2 + 1] << 8));
    }
}

// Used for raw recordings - not efficient compared to generalised or even CSW
void send_raw_block(PIO pio, int sm, t_block_desc blk, uint8_t buf[])
{
    uint8_t prev_bit = 0;
    uint8_t this_bit = 0;
    int last_bit = 0;
    uint32_t ticks = 0;

    for (uint32_t x = 0; x < blk.blklen; x++)
    {
        if ((x == blk.blklen - 1) && (blk.used_bits != 8))
        {
            last_bit = 8 - blk.used_bits;
        }

        for (int y = 8; --y >= last_bit;)
        {
            this_bit = ((buf[x] >> y) & 0x1);
            if (ticks == 0)
            {
                // Start the hold
                ticks = blk.sample_ticks;
                prev_bit = this_bit;
            }
            else if (this_bit != prev_bit)
            {
                // Take off the hold, send the sample, hold at new level
                send_pulse(pio, sm, ticks);
                ticks = blk.sample_ticks;
                prev_bit = this_bit;
            }
            else
            {
                // Keep holding
                ticks = ticks + blk.sample_ticks;
            }
        }
    }
}

void send_standard_block(PIO pio, int sm, t_block_desc blk, uint8_t buf[])
{
    // Pilot / Sync if the block is not Pure Data
    if (blk.type != 0x14)
    {
        // Pilot tone
        for (uint32_t i = 0; i < blk.p_total; i++)
        {
            send_pulse(pio, sm, blk.p_pulse);
        }

        // Sync - just two pulses
        send_pulse(pio, sm, blk.sync_a);
        send_pulse(pio, sm, blk.sync_b);
    }

    /*
     * Payload
     */
    // Process the bytes...
    for (uint32_t x = 0; x < blk.blklen; x++)
    {
        // Process the bits in the byte, MSB first..
        int last_bit = 0;
        if ((x == blk.blklen - 1) && (blk.used_bits != 8))
        {
            last_bit = 8 - blk.used_bits;
        }

        for (int y = 8; --y >= last_bit;)
        {
            // Play the appropriate wave for 1 or 0
            if ((buf[x] >> y) & 0x1)
            {
                send_pulse(pio, sm, blk.bit_one);
                send_pulse(pio, sm, blk.bit_one);
            }
            else
            {
                send_pulse(pio, sm, blk.bit_zero);
                send_pulse(pio, sm, blk.bit_zero);
            }
        }
    }
}

void send_csw_block(PIO pio, int sm, t_block_desc blk, uint8_t buf[])
{
    uint8_t *pulses = NULL;
    uint8_t *d_rle = NULL;
    int32_t s_rle = -1;
    // number is 16 bit
    pulses = malloc(2 * blk.d_total);

    /*
     * (1) Normal RLE of 5 short pulses:
     *  03 05 01 04 07
     * ___-----_----_
     *
     *  A pulse longer than 0xFF / 255 t-states:
     *  - start with 0x00
     *  - then 4 bytes 00 60 BF 93 01 = 600 secs @ 44100Hz
     *
     * (2) Z-RLE is RLE but compressed with zlib deflate
     */
    if (blk.compression > 1)
    {
        // Z-RLE - Decompress / Inflate
        // Assume most samples are 5 bytes (>255 t-states)
        d_rle = malloc(5 * 2 * blk.d_total);
#ifdef USE_ZLIB
        s_rle = inflate_zlib(buf, blk.blklen - 11, d_rle, 5 * 2 * blk.d_total);
#endif
        // If inflate_zlib fails, exit gracefully
        if (s_rle < 0)
        {
            free(d_rle);
            free(pulses);
            return;
        }
    }
    else
    {
        // RLE - No decompression needed
        s_rle = blk.blklen - 11;
        d_rle = malloc(s_rle);
        d_rle = buf;
    }

    // Convert RLE to array of uint16_t pulse lengths
    uint16_t y = 0;
    for (uint32_t i = 0; i < s_rle; i++)
    {
        if (d_rle[i] != 0x00)
        {
            pulses[y++] = d_rle[i];
            pulses[y++] = 0x00;
        }
        else
        {
            // Take the next two bytes after the 0x00
            pulses[y++] = d_rle[++i];
            pulses[y++] = d_rle[++i];

            // Drop the 2 MSBs as it's unlikely to be > 1sec
            // If not...  rewrite the code to be 32-bit aware
            i += 2;
        }
    }
    // Function processes the CSW now as an array of pulses
    send_pulse_array(pio, sm, blk.d_total, pulses);

    free(d_rle);
    free(pulses);
}

void send_gen_block(PIO pio, int sm, t_block_desc blk, uint8_t buf[])
{
    // Address tracker, as everything else is now dynamic
    uint32_t dynaddr = 0;

    // For the data playback which is **bit-based**
    uint8_t bit_size = ceil(log2(blk.d_symbols)); // 1-8 bits
    uint16_t bit_cache = 0;                       // holds bits left over
    uint8_t bit_left = 0;                         // how many bits left over

    /*
     * Generalised Pilot / Sync Playback
     */
    if (blk.p_total > 0)
    {
        // Symbols Def - Pilot/Sync
        // Storage: [symbol #][level, tstates, tstates...]
        uint16_t(*p_sym_list)[blk.p_max_pulses + 1] =
            malloc(sizeof(uint16_t[blk.p_symbols][blk.p_max_pulses + 1]));

        // For each symbol in the alphabet
        for (uint8_t x = 0; x < blk.p_symbols; x++)
        {
            // Get the flags for the symbol
            p_sym_list[x][0] = buf[dynaddr++];
            // Get the pulse sequence for the symbol
            for (uint8_t y = 0; y < blk.p_max_pulses; y++)
            {
                p_sym_list[x][y + 1] = buf[dynaddr++] | buf[dynaddr++] << 8;
            }
        }

        // Data RLE - Pilot/Sync
        // For each 3 bytes that make the data
        for (uint32_t x = 0; x < blk.p_total; x++)
        {
            // Get the symbol value
            uint8_t symdef = buf[dynaddr++];
            // Get the repeat value
            uint16_t repeat = buf[dynaddr++] | buf[dynaddr++] << 8;

            /*
             *   Signal Level Change
             *   0 = opposite current state (do nothing)
             *   1 = hold current state
             *   2 = force high
             *   3 = force low
             */
            switch (p_sym_list[symdef][0])
            {
            case 0x01:
                // Get it back to what it was last
                send_pulse(pio, sm, 0);
                break;
            case 0x02:
                // Currently LOW, need HIGH
                if (gpio_level == 0)
                {
                    send_pulse(pio, sm, 0);
                }
                break;
            case 0x03:
                // Currently HIGH, need LOW again
                if (gpio_level == 1)
                {
                    send_pulse(pio, sm, 0);
                }
                break;
            case 0x00:
                // Do nothing
            default:
            }

            // Repeat the pilot / sync sequence per the vars
            for (uint16_t y = 0; y < repeat; y++)
            {
                // go thru the symdef sequence
                for (uint16_t z = 0; z < blk.p_max_pulses; z++)
                {
                    // if there's no sample (i.e. 0), skip
                    if (p_sym_list[symdef][z + 1] > 0)
                    {
                        // send the 16-bit pulse
                        send_pulse(pio, sm, p_sym_list[symdef][z + 1]);
                    }
                }
            }
        }
        free(p_sym_list);
    }

    /*
     * Generalised Data Playback
     */
    if (blk.d_total > 0)
    {
        // Symbols Def - Data
        // Storage: [symbol #][level, tstates, tstates...]
        uint16_t(*d_sym_list)[blk.d_max_pulses + 1] =
            malloc(sizeof(uint16_t[blk.d_symbols][blk.d_max_pulses + 1]));

        // For each symbol in the alphabet
        for (uint8_t x = 0; x < blk.d_symbols; x++)
        {
            // Get the flags for the symbol
            d_sym_list[x][0] = buf[dynaddr++];
            // Get the pulse sequence for the symbol
            for (uint8_t y = 0; y < blk.d_max_pulses; y++)
            {
                d_sym_list[x][y + 1] = buf[dynaddr++] | buf[dynaddr++] << 8;
            }
        }

        // Data is different - it's done by bits, not bytes and no RLE repeating
        while (blk.d_total > 0)
        {
            // If we don't have enough bits to look up the symbol
            if (bit_left < bit_size)
            {
                // Shift up the previous value
                bit_cache <<= 8;
                // Pull in another 8 bits
                bit_cache |= buf[dynaddr++];
                // state we've added more bits
                bit_left += 8;
            }
            // Adjust the bit_left in advance, as we use it now
            bit_left -= bit_size;
            // Shift & mask the bits into focus - this will look up the symbol
            uint8_t symdef = (bit_cache >> bit_left) & ((1 << bit_size) - 1);

            /*
             *   Signal Level Change
             *   0 = opposite current state (do nothing)
             *   1 = hold current state
             *   2 = force high
             *   3 = force low
             */
            switch (d_sym_list[symdef][0])
            {
            case 0x00:
                break;
            case 0x01:
                // Currently X, will be Y, but need X
                send_pulse(pio, sm, 0);
                break;
            case 0x02:
                // Currently HIGH, will be LOW, need HIGH again
                if (gpio_level == 1)
                {
                    send_pulse(pio, sm, 0);
                }
                break;
            case 0x03:
                // Currently LOW, will be HIGH, need LOW again
                if (gpio_level == 0)
                {
                    send_pulse(pio, sm, 0);
                }
                break;
            default:
            }

            // Cycle thru the symbols sequence
            for (uint16_t z = 0; z < blk.d_max_pulses; z++)
            {
                // if there's no sample (i.e. 0), skip
                if (d_sym_list[symdef][z + 1] > 0)
                {
                    // send the representative 16-bit pulse
                    send_pulse(pio, sm, d_sym_list[symdef][z + 1]);
                }
            }
            // Mask only the bits left
            bit_cache &= ((1 << bit_left) - 1);
            // d_total - decrement the number of data points
            blk.d_total--;
        }
        free(d_sym_list);
    }
}

int main()
{
    uint32_t tapesize;
    // Allow stdout/stdin
    stdio_init_all();
    // Pause so we can hook up the stdout
    sleep_ms(10000);

    // Prep the PIO and GPIO
    PIO pio = pio0;
    assert(AUDIO_PIN < 31);

#ifdef USE_FATFS
    // Get the TZX data off the SD card
    uint8_t *tape = NULL;
    tapesize = get_tzx_from_sdcard(&tape, FILENAME);
#else
    tapesize = sizeof(tape);
#endif
    uint32_t *block_start = validate_tape(tape, tapesize);

    // Pulse generator PIO program - allocate to PIO and State Machine (SM)
    uint pulsegen_offset;
    if (pio_can_add_program(pio, &pulsegen_program))
    {
        pulsegen_offset = pio_add_program(pio, &pulsegen_program);
    }
    else
    {
        return -1;
    }
    int pulsegen_sm = pio_claim_unused_sm(pio, true);
    if (pulsegen_sm == -1)
    {
        return -1;
    }

    // Initialise the state machine with PIO, SM, offset, GPIO and clock speed.
    float freq = (float)clock_get_hz(clk_sys) / FREQ;
    pulsegen_program_init(pio, pulsegen_sm, pulsegen_offset, AUDIO_PIN, freq);

    // Turn on the state machine
    pio_sm_set_enabled(pio, pulsegen_sm, true);

    // Keep looping
    while (true)
    {
        sleep_ms(10000);

        // Vars for blocks, bytes
        uint16_t block = 0;
        uint32_t addr = 0;

        // Vars for looping / sequences - not commonly used
        uint8_t seq_live = 0, seq_paused = 0;
        int16_t *seq_list = NULL;
        uint16_t seq_step = 0, seq_size;
        uint32_t loop_start, loop_count, seq_return;

        printf("Starting tape playback...\n");

        // While we have data...
        while (block_start[block] != UINT32_MAX)
        {
            // Set block defaults
            t_block_desc blk = {0x10, 0, 2168, 667, 735, 885, 1710, 8};

            // Sequence handling if sequence is live
            if ((seq_live) && (seq_step < seq_size))
            {
                // may be relative to the start... if so calculate at sequence
                block = seq_list[seq_step++];
            }
            else if (seq_live)
            {
                // should never reach here - should hit 0x27
                seq_live = 0;
                block = seq_return;
            }

            // Go to the block
            addr = block_start[block];
            // printf("%u: ID=%02x, L=%u\n", block, tape[addr], gpio_level);

            // Go through the TZX block types supported
            if ((tape[0] == 0x5A) && (tape[1] == 0x58) && (tape[2] == 0x54))
            {
                switch (tape[addr])
                {
                /*
                 * Data Blocks
                 */
                case BLK_STD:
                    blk.type = tape[addr];

                    // Next four bytes contain the pause and data length
                    blk.pause = tape[addr + 1] | tape[addr + 2] << 8;
                    blk.blklen = tape[addr + 3] | tape[addr + 4] << 8;

                    // Sixth marker byte dictates the pilot length
                    if (tape[addr + 5] >= 0x80)
                        blk.p_total = 3223; // Data block (0xFF)
                    else
                        blk.p_total = 8063; // Header block (0x00)

                    send_standard_block(pio, pulsegen_sm, blk, tape + addr + 5);

                    break;

                // Turbo Speed
                case BLK_TURBO:
                    blk.type = tape[addr];
                    // Have to define everything from the tape
                    blk.p_pulse = tape[addr + 1] | tape[addr + 2] << 8;
                    blk.sync_a = tape[addr + 3] | tape[addr + 4] << 8;
                    blk.sync_b = tape[addr + 5] | tape[addr + 6] << 8;
                    blk.bit_zero = tape[addr + 7] | tape[addr + 8] << 8;
                    blk.bit_one = tape[addr + 9] | tape[addr + 10] << 8;
                    blk.p_total = tape[addr + 11] | tape[addr + 12] << 8;
                    blk.used_bits = tape[addr + 13];
                    blk.pause = tape[addr + 14] | tape[addr + 15] << 8;
                    blk.blklen = tape[addr + 16] | tape[addr + 17] << 8 | tape[addr + 18] << 16;

                    send_standard_block(pio, pulsegen_sm, blk, tape + addr + 19);

                    break;

                // Pure Tone
                case BLK_TONE:
                    blk.bit_zero = tape[addr + 1] | tape[addr + 2] << 8;
                    blk.blklen = tape[addr + 3] | tape[addr + 4] << 8;
                    send_pure_tone_block(pio, pulsegen_sm, (uint16_t)blk.blklen, blk.bit_zero);
                    break;

                // Pulse Sequence
                case BLK_PULSES:
                    send_pulse_array(pio, pulsegen_sm, tape[addr + 1], tape + addr + 2);
                    break;

                // Pure Data
                case BLK_PDATA:
                    blk.type = tape[addr];
                    blk.bit_zero = tape[addr + 1] | tape[addr + 2] << 8;
                    blk.bit_one = tape[addr + 3] | tape[addr + 4] << 8;
                    blk.used_bits = tape[addr + 5];
                    blk.pause = tape[addr + 6] | tape[addr + 7] << 8;
                    blk.blklen = tape[addr + 8] | tape[addr + 9] << 8 | tape[addr + 10] << 16;

                    send_standard_block(pio, pulsegen_sm, blk, tape + addr + 11);

                    break;

                // Direct Recording (Sampled)
                case BLK_DIRECT:
                    blk.type = tape[addr];
                    blk.sample_ticks = tape[addr + 1] | tape[addr + 2] << 8;
                    blk.pause = tape[addr + 3] | tape[addr + 4] << 8;
                    blk.used_bits = tape[addr + 5];
                    blk.blklen = tape[addr + 6] | tape[addr + 7] << 8 | tape[addr + 8] << 16;

                    send_raw_block(pio, pulsegen_sm, blk, tape + addr + 9);

                    break;

                // Compressed Square Wave
                case BLK_CSW:
                    blk.blklen = tape[addr + 1] | tape[addr + 2] << 8 | tape[addr + 3] << 16 | tape[addr + 4] << 24;
                    blk.pause = tape[addr + 5] | tape[addr + 6] << 8;

                    // ZX Spectrum Hz / Sample Hz = t-state length
                    blk.sample_ticks = FREQ / (tape[addr + 7] | tape[addr + 8] << 8 | tape[addr + 9] << 16);

                    // RLE or Z-RLE
                    blk.compression = tape[addr + 10];

                    // Number of samples
                    blk.d_total = tape[addr + 11] | tape[addr + 12] << 8 | tape[addr + 13] << 16 | tape[addr + 14] << 24;

                    send_csw_block(pio, pulsegen_sm, blk, tape + addr + 15);

                    break;

                // Generalised - horrible mix of everything
                case BLK_GENERAL:
                    blk.blklen = tape[addr + 1] | tape[addr + 2] << 8 | tape[addr + 3] << 16 | tape[addr + 4] << 24;
                    blk.pause = tape[addr + 5] | tape[addr + 6] << 8;

                    // Pilot / Sync
                    blk.p_total = tape[addr + 7] | tape[addr + 8] << 8 | tape[addr + 9] << 16 | tape[addr + 10] << 24;
                    blk.p_max_pulses = tape[addr + 11];
                    blk.p_symbols = tape[addr + 12];

                    // Data
                    blk.d_total = tape[addr + 13] | tape[addr + 14] << 8 | tape[addr + 15] << 16 | tape[addr + 16] << 24;
                    blk.d_max_pulses = tape[addr + 17];
                    blk.d_symbols = tape[addr + 18];

                    send_gen_block(pio, pulsegen_sm, blk, tape + addr + 19);

                    break;

                /*
                 * Behaviour Blocks
                 */
                case BLK_PAUSE:
                    blk.pause = (tape[addr + 1] | tape[addr + 2] << 8) * 1000;
                    break;

                // Treated as a contiguous block for sequences
                case BLK_GROUP_START:
                    // Pause the sequence if needed
                    if (seq_live)
                    {
                        seq_paused = 1;
                        seq_live = 0;
                    }
                    break;
                case BLK_GROUP_END:
                    // Unpause sequence if needed
                    if (seq_paused)
                    {
                        seq_live = 1;
                    }
                    break;

                // Jump - Signed short word
                case BLK_JUMP:
                {
                    // Need to figure out how a signed 16-bit int works
                    int16_t offset = tape[addr + 1] | tape[addr + 2] << 8;

                    // e.g. this is 5, next will be 6, but offset is -2
                    //   6 += (-2) - 1 = 3
                    block += offset - 1;
                    break;
                }

                // Loop
                case BLK_LOOP_START:
                    loop_count = tape[addr + 1] | tape[addr + 2] << 8;
                    loop_start = block;
                    break;

                // Loop end
                case BLK_LOOP_END:
                    // Keep going back until counter is run down
                    if (--loop_count > 0)
                    {
                        // Will be +1 after loop
                        block = loop_start;
                    }
                    break;

                // Sequence array
                case BLK_SEQ_CALL:
                    seq_size = tape[addr + 1] | tape[addr + 2] << 8;

                    seq_list = malloc(seq_size * sizeof(int16_t));
                    seq_live = 1;
                    seq_step = 0;
                    seq_return = block;

                    // if not relative to each other, and just to start...
                    for (uint16_t x = 0; x < seq_size; x++)
                    {
                        seq_list[x] = block + (tape[addr + 3 + (2 * x)] | tape[addr + 4 + (2 * x)] << 8);
                    }

                    break;

                case BLK_SEQ_RET:
                    // Return to where we left off
                    block = seq_return;

                    // Clean up
                    seq_live = 0;
                    free(seq_list);
                    seq_list = NULL;
                    break;

                case BLK_SEL:
                    // May need buttons to advance the tape

                    // Build the menu
                    uint8_t menu_size = tape[addr + 3];
                    int16_t *offset = NULL;
                    offset = malloc(sizeof(int16_t) * menu_size);
                    char(*names)[31] = malloc(sizeof(char[menu_size][31]));
                    int x = 4;
                    for (uint8_t y = 0; y < menu_size; y++)
                    {
                        offset[y] = tape[addr + x] | tape[addr + x + 1] << 8;
                        memcpy(names[y], &tape[addr + x + 3], tape[addr + x + 2]);
                        names[y][tape[addr + x + 2]] = '\0';
                        x += tape[addr + x + 2] + 3;
                        printf("%u) %s @ %d\n", y, names[y], block + offset[y]);
                    }

                    free(offset);
                    free(names);

                    break;

                case BLK_STOP_48K:
                    // No way for us to detect the hardware
                    break;

                case BLK_SIG_LEVEL:
                    // This might not be accurate if a sleep has happened
                    // If the value doesn't match last gpio_level
                    if (gpio_level == tape[addr + 5])
                        send_pulse(pio, pulsegen_sm, 0);
                    break;

                /*
                 * Info Blocks - Mostly nothing to do
                 */
                case BLK_TEXT:
                    break;
                case BLK_MSG:
                    blk.pause = tape[addr + 1] * 1000;
                    sleep_ms(blk.pause);
                    break;
                case BLK_INFO:
                case BLK_HARDWARE:
                case BLK_CUSTOM:
                case BLK_GLUE:
                    break;

                default:
                }
            }
            else
            {   // Otherwise treat it like a TAP file
                // First two bytes are the size
                blk.blklen = tape[addr] | tape[addr + 1] << 8;

                // Third marker byte dictates the pilot length
                if (tape[addr + 2] >= 0x80)
                    blk.p_total = 3223; // Data block
                else
                    blk.p_total = 8063; // Header block

                // Send for processing
                send_standard_block(pio, pulsegen_sm, blk, tape + addr + 2);

                // Set a default ause
                blk.pause = 1000;
            }

            // Increment to next block
            block++;

            // Pause as required
            if (blk.pause > 0)
                sleep_ms(blk.pause);
        }

        // Clean up memory
        free(block_start);
        block_start = NULL;

        // End playback and pause for 30 secs
        printf("End of tape after: %u bytes\n\n", tapesize);
        sleep_ms(30000);
    }
}
