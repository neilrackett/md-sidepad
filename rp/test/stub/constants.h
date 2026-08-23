#ifndef STUB_CONSTANTS_H
#define STUB_CONSTANTS_H
extern unsigned char xpadtest_rom[];
#define __rom_in_ram_start__ (*(unsigned int *)xpadtest_rom)
#endif
