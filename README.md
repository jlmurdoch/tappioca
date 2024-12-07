# TAPPIOCA - Tape PIO Cassette Audio

Play back ZX Spectrum TZX / TAP files over GPIO-powered audio using the [RP2040/RP2350 PIO capability](https://www.raspberrypi.com/news/what-is-pio/) for precise timing, relaying the audio into the EAR socket on a ZX Spectrum.

## Build
(Optional) Add the following 3rd-party libraries into the sources directory if needed:
- [no-OS-FatFS-SD-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico) - for SD Card support for storing TZX and TAP files.
- [zlib-deflate-nostdlib](https://github.com/derf/zlib-deflate-nostdlib/) - for Zlib inflate support need by some rare CSW data blocks.

Then configure and build:
```
mkdir build
cd build
# Build with all optional libraries and for RP2350
cmake .. -DUSE_ZLIB=ON -DUSE_FATFS=ON -DPICO_PLATFORM=rp2350 
make
```

## Install
To upload the uf2 image, for example when using a Raspberry Pi Pico, the bootloader button should be held when powered on / reset, to present a drive, which can then be mounted:
```
mount /dev/sdX1 /mnt
cp tappioca.uf2 /mnt
umount /mnt
```

## Why do this in the first place?
Most DivMMC clones can only play TAP files, which don't support complex loading schemes which TZX can.

## Why use the RP2040/RP2350 PIO?
RP2040/RP2350 PIO state machines can run assembly code aligned to the ZX Spectrum clock speed of 3.5MHz. It should have no issues playing TZX files that contain complex / timing-sensitive tape loading schemes.

## Why not add support for other microcontrollers?
PIO is used in lieu of timer and interrupt code. Offloading the raw pulse work to the PIO leaves the microcontroller to go fetch and prep more pulses for the PIO, rather than handle interrupts.

## Won't assembler over-complicate things?
It uses only 8 x 16 bit assembler instructions.

## How are TAP / TZX files incorporated into the software?
Tape files can either be stored in RP2040/RP2350 memory or retrieved from an SD Card. There are two sample files with short TAP / TZX file examples if one does not have external storage.

## What hardware is needed?
- RP2040/RP2350-based microcontroller.
- Stereo audio cable with 3.5mm jacks (3-contact 1/8" TRS)
- Board or circuit that can output audio from the GPIO. For example:
  - [Olimex RP2040-PICO-PC](https://www.olimex.com/Products/MicroPython/RP2040-PICO-PC/open-source-hardware)
  - [Simple analogue circuit](https://gregchadwick.co.uk/blog/playing-with-the-pico-pt3/). 
Below on the left, is a breadboard with 100 Ohm input resistor, voltage divider using 1910 Ohm / 1000 Ohm resistors, a 10nF capacitor, and a 5-pin 3.5mm TRRS surface-mounted socket, hooked up with leads to ground and GPIO28. On the right is the Olimex RP2040 PICO PC. Both solutions work. 
![Examples of a board and circuit options](/images/boards.jpg)

## What software is needed?
- [Raspberry Pi Pico SDK](https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html)
- Build environment (cmake, gcc, libstdc++, etc)
- Optional: [zlib decompressor (RFC 1950) and deflate reader (RFC 1951)](https://github.com/derf/zlib-deflate-nostdlib) for TZX files using CSW blocks compressed with Zlib RLE
- Optional: [Simple library for SD Cards on the Pico](https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico) 
- Optional: TZX or TAP files

## What is left to do? What could be added?
- General optimisation (memory, code, etc)
- Debugging printf() for troubleshooting
- Read direct from SD card rather than using memory
- Physical interaction capability (buttons)
- LCD Screen for menu / multiple TZX files

## Why are the CSW and Generalised routines so complex?
Both formats use a form of RLE - [Run-length Encoding](https://en.wikipedia.org/wiki/Run-length_encoding) - to save space. This involves some translation and unpacking before pulses can be generated. Per the Ramsoft CSW specification, the image below shows five CSW-encoded pulses stored as `0x03, 0x05, 0x01, 0x04, 0x07`:
![Visual Ramsoft example of five CSW pulses](/images/wave.gif)

However this gets **more** complex when there's more than 255 / 0xFF samples (i.e. in practically **all** cases of ZX Spectrum tape data), where a single pulse will consume **five** bytes: a leading 0x00 and four bytes. For example a single 855 t-state pulse will be `0x00, 0x57, 0x03, 0x00, 0x00`. 

## Does this work with non-standard loaders?
Yes:
- Crystal Kingdom Dizzy (Turbo Loader) and Aahku al Riscate (Speed Loader 2) work.
- A Yankee In Iraq (AstraLoad 3) loads in under a minute. 
- [Lone Wolf 3 is signal sensitive](https://sourceforge.net/p/fuse-emulator/bugs/352/), but worked when default starting polarity is set to HIGH and pulses are immediately terminated.

## Resources
### Tape Formats
Various explanations of what is inside TZX and TAP files:
- [ZXNet - Spectrum Tape Interface](https://sinclair.wiki.zxnet.co.uk/wiki/Spectrum_tape_interface)
- [ZXNet - TAP specification](https://sinclair.wiki.zxnet.co.uk/wiki/TAP_format)
- [Ramsoft TZX specification (Mirror by WorldOfSpectrum.net)](https://worldofspectrum.net/TZXformat.html)
- [Ramsoft CSW specification (Mirror by Kio)](https://k1.spdns.de/Develop/Projects/zxsp/Info/File%20Formats/CSW%20technical%20specifications.html)
- [Ramsoft CSW specification (Mirror by Grey Panther)](https://rhc14.grey-panther.net/doc/technical/specifications/csw.html)

### Raspberry Pi RP2040/RP2350 PIO
Greg Chadwick has two useful Raspberry Pi Pico blog posts on playing audio over GPIO using PWM (the circuits are very useful) and how PIO works:
- [Playing with the Pico - PWM Audio](https://gregchadwick.co.uk/blog/playing-with-the-pico-pt3/)
- [Playing with the Pico - Getting Acquianted with PIO](https://gregchadwick.co.uk/blog/playing-with-the-pico-pt4/)

Digikey has a good overview of using PIO:
- [Raspberry Pi Pico and RP2040 - C/C++ Part 3: How to use PIO](https://www.digikey.co.uk/en/maker/projects/raspberry-pi-pico-and-rp2040-cc-part-3-how-to-use-pio/123ff7700bc547c79a504858c1bd8110)

### Testing Tapes
The following three TZX files are modern, freely obtainable games that can be used to test the code and setup:
- [Yankee In Iraq](http://sky.relative-path.com/zx/25_years_in_the_making.html)
- [Aahku al Rescate](https://worldofspectrum.org/software?id=0026539)
- [Crystal Kingdom Dizzy](https://yolkfolk.com/games/crystal-kingdom-dizzy/)

The following is a complete list of block types tested and TZX images used:

| Test-case     | Title                                    | Common? | Block Types |
|---------------|------------------------------------------|---------|-------------|
| Turbo Loader  | Crystal Kingdom Dizzy (Dizzy 7/VII)      | Yes     | 10, 11, 20, 2A, 31, 32 |
| Pure Tone     | Aahku al Rescate                         | Yes     | 10, 12, 13, 14, 21, 22 |
| Generalised   | A Yankee in Iraq (2017)                  | No      | 10, 19, 32 |
| Block jumping | Hollywood Poker [48K]                    | No      | 11, 12, 14, 21, 22, 23, 24, 25, 26, 27, 32 |
| Tape Selector | Lone Wolf 3 [Menu: 1=128k, 2=48k]     | No      | 10, 11, 20, 21, 22, 28, 30 |
| Comments	    | Eskimo Capers                            | Yes     | 10, 32, 33, 35 |
| Raw data      | First Entry Simulator (from [CCS CGC 2013](https://csscgc2013.blogspot.com/))| No      | 15 |
| CSW data      | Explosion (from [CCS CGC 2020](https://www.connosoft.com/csscgc2020/))            | No      | 10, 18 |
| Signal Level  | CASIO Digit Invaders (from [CCS CGC 2020](https://www.connosoft.com/csscgc2020/)) | No      | 10, 20, 2B |
| Concatenation | (just append two TZX's using "cat")      | No      | 5A |

### Debugging

Everything should work with the above tapes, however some may slip through the net. Strategically-placed printf() statements can be used in the code, but make sure they do not block too much. If the data in the FIFO runs out (e.g. at start / during a pause), the PIO state machine with hold the current signal for longer than expected until it gets new data.

Tools used to do further in-depth analysis are:
- `tzxlist` from the [Fuse Emulator](https://fuse-emulator.sourceforge.net/) project to nspect TZX block structures.
- Exporting data using [PlayTZX](https://github.com/tcat-qldom/PlayTZX) to VOC/AU and then viewing in [Audacity](https://www.audacityteam.org/) to look at signal polarity / timings.
- Using [ZEsarUX](https://github.com/chernandezba/zesarux) with its "real tape" external audio source feature to avoid having to hook up a ZX Spectrum. It also has a near-live waveform viewer. Execute with a specific audio driver, e.g. `--ao pulse`, if you cannot see external audio source options.

![Screenshot of ZEsarUX with the waveform viewer showing a pilot tone](/images/zesarux.png)
