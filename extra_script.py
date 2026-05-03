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
clibindings   = os.path.join(src_dir, "tivawords.txt")

# Run immediately at script-load time, before SCons constructs the build
# graph.  AddPreAction fires after source scanning, so generated files
# written that way aren't seen until the next build.  Top-level execution
# here means wordlist.c and help.c are on disk before SCons decides what
# to compile.
if not os.path.isfile(parsewords):
    print("extra_script: parsewords.py not found — skipping wordlist regen")
else:
    result = subprocess.run(
        ["python3", parsewords, clibindings],
        cwd=wordlists_dir,
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("extra_script: parsewords.py failed:\n", result.stderr)
    else:
        print("Results:", result.stdout)
        # import shutil
        # for name in ("wordlist.c", "help.c"):
        #     src_file = os.path.join(wordlists_dir, name)
        #     dest     = os.path.join(src_dir, name)
        #     if os.path.isfile(src_file):
        #         shutil.copy2(src_file, dest)
        # print("extra_script: wordlist.c + help.c refreshed in", src_dir)

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
