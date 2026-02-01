#!/usr/bin/env python3
"""
Extract ZX Spectrum character set from ROM file.
Characters are stored at offset 15360 (0x3C00) in the ROM.
Each character is 8 bytes (8x8 bitmap).
There are 96 characters (ASCII 32-127).
"""

def extract_charset(rom_path, output_path):
    """Extract character set from ZX Spectrum ROM and save as C array."""
    
    CHAR_START = 15616  # 0x3D00
    CHAR_SIZE = 8       # 8 bytes per character
    NUM_CHARS = 96      # 96 characters (ASCII 32-127)
    
    try:
        with open(rom_path, 'rb') as f:
            rom_data = f.read()
        
        # Check if ROM is large enough
        if len(rom_data) < CHAR_START + (NUM_CHARS * CHAR_SIZE):
            print(f"Error: ROM file too small. Expected at least {CHAR_START + (NUM_CHARS * CHAR_SIZE)} bytes.")
            return
        
        # Extract character data
        charset = rom_data[CHAR_START:CHAR_START + (NUM_CHARS * CHAR_SIZE)]
        
        # Write C header file
        with open(output_path, 'w') as f:
            f.write("/*\n")
            f.write(" * ZX Spectrum Character Set\n")
            f.write(" * Extracted from ROM at address 0x3D00\n")
            f.write(" * 96 characters (ASCII 32-127), 8 bytes each\n")
            f.write(" */\n\n")
            f.write("const unsigned char zx_spectrum_charset[96][8] = {\n")
            
            for char_idx in range(NUM_CHARS):
                offset = char_idx * CHAR_SIZE
                char_bytes = charset[offset:offset + CHAR_SIZE]
                
                ascii_code = char_idx + 32
                char_name = chr(ascii_code) if 33 <= ascii_code <= 126 else ' '
                
                # Format the byte array
                byte_str = ', '.join(f'0x{b:02X}' for b in char_bytes)
                
                # Add comment with ASCII code and character
                if ascii_code == 32:
                    comment = f"  /* ASCII {ascii_code:3d} (SPACE) */"
                elif 33 <= ascii_code <= 126:
                    comment = f"  /* ASCII {ascii_code:3d} ('{char_name}') */"
                else:
                    comment = f"  /* ASCII {ascii_code:3d} */"
                
                # Write the line
                comma = ',' if char_idx < NUM_CHARS - 1 else ''
                f.write(f"    {{{byte_str}}}{comma}{comment}\n")
            
            f.write("};\n")
        
        print(f"Successfully extracted {NUM_CHARS} characters to {output_path}")
        
    except FileNotFoundError:
        print(f"Error: ROM file '{rom_path}' not found.")
    except Exception as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    import sys
    
    if len(sys.argv) < 2:
        print("Usage: python extract_zx_charset.py <rom_file> [output_file]")
        print("\nExample:")
        print("  python extract_zx_charset.py zx48.rom charset.h")
        sys.exit(1)
    
    rom_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "zx_charset.h"
    
    extract_charset(rom_file, output_file)