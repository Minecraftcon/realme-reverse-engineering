#!/usr/bin/env python3
"""
Magisk Module Builder & Packager for MTK Aircrack Core
Author: Shado & Antigravity
"""

import os
import sys
import hashlib
import zipfile
import subprocess
import shutil

MODULE_DIR = os.path.abspath(os.path.dirname(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(MODULE_DIR, ".."))
WARPED_KO_SRC = os.path.join(PROJECT_ROOT, "driver-warp", "wlan_drv_gen4m_warped.ko")
MODULE_KO_DEST = os.path.join(MODULE_DIR, "system", "vendor", "lib", "modules", "wlan_drv_gen4m.ko")
OUTPUT_ZIP_DIR = PROJECT_ROOT

def get_file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()

def read_module_props(prop_file):
    props = {}
    with open(prop_file, "r") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                props[k.strip()] = v.strip()
    return props

def main():
    print("=" * 60)
    print(" MTK Aircrack Core - Magisk Module Packager")
    print("=" * 60)

    # 1. Verify and sync driver
    if not os.path.exists(WARPED_KO_SRC):
        print(f"[-] Error: Warped driver not found: {WARPED_KO_SRC}")
        sys.exit(1)

    warped_hash = get_file_sha256(WARPED_KO_SRC)
    print(f"[+] Source Warped Driver SHA256: {warped_hash}")

    os.makedirs(os.path.dirname(MODULE_KO_DEST), exist_ok=True)
    shutil.copy2(WARPED_KO_SRC, MODULE_KO_DEST)
    print(f"[+] Synced warped driver to: {MODULE_KO_DEST}")

    # 2. Set file permissions on module files
    for root, dirs, files in os.walk(MODULE_DIR):
        for d in dirs:
            os.chmod(os.path.join(root, d), 0o755)
        for file in files:
            p = os.path.join(root, file)
            if os.path.islink(p):
                continue
            if "bin" in root or file.endswith(".sh") or file == "update-binary":
                os.chmod(p, 0o755)
            else:
                os.chmod(p, 0o644)

    # 3. Read module details
    prop_path = os.path.join(MODULE_DIR, "module.prop")
    if not os.path.exists(prop_path):
        print("[-] Error: module.prop not found!")
        sys.exit(1)

    props = read_module_props(prop_path)
    mod_id = props.get("id", "mtk-aircrack-core")
    mod_ver = props.get("version", "v1.1.0")
    mod_name = props.get("name", "MTK Aircrack Core")
    print(f"[+] Module ID:   {mod_id}")
    print(f"[+] Name:        {mod_name}")
    print(f"[+] Version:     {mod_ver}")

    # 4. Create ZIP package
    zip_filename = f"{mod_id}-{mod_ver}.zip"
    zip_path = os.path.join(OUTPUT_ZIP_DIR, zip_filename)

    print(f"[*] Packaging into: {zip_path} ...")
    excluded_files = {".git", ".gitignore", "build_module.py", "__pycache__"}

    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(MODULE_DIR):
            dirs[:] = [d for d in dirs if d not in excluded_files]
            for file in files:
                if file in excluded_files or file.endswith(".pyc") or file.endswith(".zip"):
                    continue
                full_path = os.path.join(root, file)
                rel_path = os.path.relpath(full_path, MODULE_DIR)
                if os.path.islink(full_path):
                    link_target = os.readlink(full_path)
                    zinfo = zipfile.ZipInfo(rel_path)
                    zinfo.create_system = 3 # Unix
                    zinfo.external_attr = 0o120777 << 16 # S_IFLNK | 0777
                    zf.writestr(zinfo, link_target)
                    print(f"    added (symlink): {rel_path} -> {link_target}")
                else:
                    zf.write(full_path, arcname=rel_path)
                    print(f"    added: {rel_path}")

    # 5. Verify ZIP integrity
    zip_size = os.path.getsize(zip_path)
    zip_hash = get_file_sha256(zip_path)
    print(f"[+] Package created successfully!")
    print(f"[+] File:   {zip_path} ({zip_size} bytes)")
    print(f"[+] SHA256: {zip_hash}")

    # Write SHA256 checksum file
    sha256_path = f"{zip_path}.sha256"
    with open(sha256_path, "w") as f:
        f.write(f"{zip_hash}  {zip_filename}\n")
    print(f"[+] Checksum file: {sha256_path}")

    # Test with unzip
    res = subprocess.run(["unzip", "-t", zip_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode == 0:
        print("[+] ZIP Integrity: Verified OK (0 errors).")
    else:
        print(f"[-] ZIP Integrity Check Failed:\n{res.stderr}")
        sys.exit(1)

    print("=" * 60)
    print(f" Ready to deploy: {zip_filename}")
    print("=" * 60)

if __name__ == "__main__":
    main()
