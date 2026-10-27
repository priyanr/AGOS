import subprocess
import time
import os
import sys

img_path = "/tmp/test_installed.img"
if os.path.exists(img_path):
    os.remove(img_path)

subprocess.run(["qemu-img", "create", "-f", "raw", img_path, "256M"], check=True)

print("===================================================", flush=True)
print("=== 1. INSTALLING ANTIGRAVITY OS TO SATA DISK   ===", flush=True)
print("===================================================", flush=True)

p = subprocess.Popen([
    "qemu-system-x86_64",
    "-boot", "d",
    "-cdrom", "/mnt/f/priyan/Projects/AI/OS/antigravity-os.iso",
    "-device", "ahci,id=ahci",
    "-drive", f"id=satadisk,file={img_path},if=none,format=raw",
    "-device", "ide-hd,drive=satadisk,bus=ahci.0",
    "-serial", "stdio",
    "-display", "none"
], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

time.sleep(2)
p.stdin.write("install sda\n")
p.stdin.flush()

start = time.time()
completed = False
while time.time() - start < 30:
    line = p.stdout.readline()
    if not line:
        break
    print(line, end="", flush=True)
    if "Installation Completed Successfully" in line:
        completed = True
        print("\n[OK] Installation wizard completed successfully!", flush=True)
        break

p.terminate()
p.communicate()

if not completed:
    print("[FAIL] Installer did not complete.", flush=True)
    sys.exit(1)

time.sleep(1)

print("\n===================================================", flush=True)
print("=== 2. BOOTING DIRECTLY FROM INSTALLED DISK     ===", flush=True)
print("===================================================", flush=True)

p2 = subprocess.Popen([
    "qemu-system-x86_64",
    "-device", "ahci,id=ahci",
    "-drive", f"id=satadisk,file={img_path},if=none,format=raw",
    "-device", "ide-hd,drive=satadisk,bus=ahci.0",
    "-serial", "stdio",
    "-display", "none"
], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

time.sleep(2)
p2.stdin.write("ls /\n")
p2.stdin.flush()
time.sleep(1)
p2.stdin.write("cat /readme.txt\n")
p2.stdin.flush()
time.sleep(1)

start = time.time()
booted = False
while time.time() - start < 15:
    line = p2.stdout.readline()
    if not line:
        break
    print(line, end="", flush=True)
    if "Welcome to Antigravity OS!" in line:
        booted = True
    if "This installation was deployed directly" in line:
        break

p2.terminate()
p2.communicate()

if booted:
    print("\n[SUCCESS] Antigravity OS booted successfully from installed hard disk!", flush=True)
else:
    print("\n[FAIL] Direct boot failed.", flush=True)
    sys.exit(1)
