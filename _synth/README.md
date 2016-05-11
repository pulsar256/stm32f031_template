# Firmware for "polyphonic-fm-synthesizer"

Fimrware for the "polyphonic-fm-synthesizer" reference platform:

http://blog.kehribar.me/build/2015/12/06/polyphonic-fm-synthesizer-with-stm32f031.html

## Build Environment

### Pre-Requisites

* gcc-arm-none-eabi-5_3-2016q1
* Build Tools (make)
* OpenOCD 0.9.0
* SWD / ST-Link V2 Adapter

### Building

use make to build the firmware, do not forget to add the gcc-arm toolchain's binary foder to your system environment path:

```bash
➜  _synth git:(development) ✗ export PATH=~/development/gcc-arm-none-eabi-5_3-2016q1/bin:$PATH
➜  _synth git:(development) ✗ make
(...)
arm-none-eabi-size obj/main.elf
   text    data     bss     dec     hex filename
  14648      40    1608   16296    3fa8 obj/main.e
```

### Flashing

```bash
➜  _synth git:(development) ✗ make openocd-flash
openocd -f ../BackendSupport/openocd.cfg -c flash_chip
(...)
verified 14688 bytes in 0.243762s (58.843 KiB/s)
shutdown command invoked
➜  _synth git:(development) ✗
```
