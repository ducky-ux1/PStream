import sys
import struct

def clean_oelf(input_path, output_path):
    with open(input_path, 'rb') as f:
        data = bytearray(f.read())
    
    if data[:4] != b'\x7fELF':
        raise ValueError("Not an ELF file")
    
    e_phoff = struct.unpack_from('<Q', data, 0x20)[0]
    e_phentsize = struct.unpack_from('<H', data, 0x36)[0]
    e_phnum = struct.unpack_from('<H', data, 0x38)[0]
    
    print(f"Reading {input_path}: phoff=0x{e_phoff:x}, phentsize={e_phentsize}, phnum={e_phnum}")
    
    headers = []
    seen = set()
    duplicates = []
    
    for i in range(e_phnum):
        offset = e_phoff + i * e_phentsize
        hdr = data[offset : offset + e_phentsize]
        p_type, p_flags, p_foff, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack('<2I6Q', hdr)
        key = (p_type, p_vaddr, p_foff, p_filesz, p_memsz)
        if p_type == 1 and key in seen: # Duplicate PT_LOAD
            print(f"Found duplicate PT_LOAD at index {i}: vaddr=0x{p_vaddr:x}, foff=0x{p_foff:x}")
            duplicates.append(i)
        else:
            seen.add(key)
            headers.append(hdr)
    
    if not duplicates:
        print("No duplicate PT_LOAD headers found.")
    else:
        new_phnum = len(headers)
        # Write packed headers back
        new_table = b''.join(headers)
        data[e_phoff : e_phoff + len(new_table)] = new_table
        # Update phnum in ELF header
        struct.pack_into('<H', data, 0x38, new_phnum)
        print(f"Removed {len(duplicates)} duplicate(s). New phnum={new_phnum}")
    
    with open(output_path, 'wb') as f:
        f.write(data)
    print(f"Wrote clean OELF to {output_path}")

if __name__ == '__main__':
    clean_oelf(sys.argv[1], sys.argv[2])
