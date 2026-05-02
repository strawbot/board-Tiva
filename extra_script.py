import os
import subprocess
from SCons.Script import Import

Import("env")

# ── Pre-build: regenerate wordlist.c and help.c from TimbreOS word lists ──
#
# parsewords.py (in TimbreOS/WordLists/) reads clibindings.txt and writes
# wordlist.c + help.c into that same directory.  We then copy the results
# into TIVA/src/ where the build picks them up.
#
# wordlist.c and help.c are also checked in under TIVA/src/ so the build
# works without Python when the word lists have not changed.  Regeneration
# runs automatically when platformio builds.

project_dir   = env["PROJECT_DIR"]
timbre_dir    = os.path.join(project_dir, "..", "TimbreOS")
wordlists_dir = os.path.join(timbre_dir, "WordLists")
src_dir       = os.path.join(project_dir, "src")
parsewords    = os.path.join(wordlists_dir, "parsewords.py")
clibindings   = os.path.join(wordlists_dir, "clibindings.txt")

def regen_wordlist(source, target, env):
    if not os.path.isfile(parsewords):
        print("extra_script: parsewords.py not found — skipping")
        return
    result = subprocess.run(
        ["python3", parsewords, clibindings],
        cwd=wordlists_dir,
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("extra_script: parsewords.py failed:\n", result.stderr)
        return
    # Copy generated files into TIVA/src/
    for name in ("wordlist.c", "help.c"):
        src  = os.path.join(wordlists_dir, name)
        dest = os.path.join(src_dir, name)
        if os.path.isfile(src):
            import shutil
            shutil.copy2(src, dest)
    print("extra_script: wordlist.c + help.c refreshed in", src_dir)

env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", regen_wordlist)

# ── Post-build: produce firmware.bin and firmware.hex ─────────────────────

def create_hex(bin_path, env):
    hex_path = env.subst("$BUILD_DIR") + "/firmware.hex"
    cmd = "arm-none-eabi-objcopy -I binary -O ihex {bin} {hex}".format(
        bin=bin_path, hex=hex_path)
    print("Creating hex:", cmd)
    env.Execute(cmd)

def create_bin(source, target, env):
    elf = str(source[0])
    bin_path = env.subst("$BUILD_DIR") + "/firmware.bin"
    cmd = "arm-none-eabi-objcopy -O binary {elf} {bin}".format(
        elf=elf, bin=bin_path)
    print("Creating binary:", cmd)
    env.Execute(cmd)
    create_hex(bin_path, env)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", create_bin)
