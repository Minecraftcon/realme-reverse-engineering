#!/usr/bin/env python3
import sys
import capstone

TEXT_OFFSET = 0x44

def disasm_func(elf_path, func_offset, func_size, base_addr=0):
    with open(elf_path, "rb") as f:
        f.seek(TEXT_OFFSET + func_offset)
        code = f.read(func_size)
    
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for ins in md.disasm(code, base_addr + func_offset):
        print(f"0x{ins.address:08x}:  {ins.mnemonic:<8} {ins.op_str}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: disasm.py <offset_hex> <size_hex>")
        sys.exit(1)
    elf = "/home/shado/Documents/realme-reverse-engineering/RMX3171/bluetooth/dumped/bt_drv.ko"
    offset = int(sys.argv[1], 16)
    size = int(sys.argv[2], 16)
    disasm_func(elf, offset, size)
