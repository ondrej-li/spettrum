#include <stdio.h>
#include <stdint.h>

int main() {
    uint8_t flags = 0x00;
    uint8_t Z80_FLAG_Z = 0x40;
    uint8_t Z80_FLAG_C = 0x01;
    
    uint8_t val = 0x08;  // 00001000
    int bit_pos = 3;
    int bit_set = (val >> bit_pos) & 1;
    
    flags = (flags & Z80_FLAG_C) | 0x10; // Set H flag
    if (!bit_set) flags |= Z80_FLAG_Z;
    
    printf("bit_set = %d\n", bit_set);
    printf("flags = 0x%02X\n", flags);
    printf("Z flag set? %d\n", flags & Z80_FLAG_Z);
    
    return 0;
}
