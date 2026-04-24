from SCons.Script import Import, AlwaysBuild, Builder

Import("env")

def create_hex(bin, env):
    hex_path = env.subst("$BUILD_DIR") + "/firmware.hex"
    cmd = "arm-none-eabi-objcopy -I binary -O ihex {bin} {hex}".format(bin=bin, hex=hex_path)
    print("Creating hex:", cmd)
    env.Execute(cmd)
    return None

def create_bin(source, target, env):
    elf = str(source[0])
    bin_path = env.subst("$BUILD_DIR") + "/firmware.bin"
    cmd = "arm-none-eabi-objcopy -O binary {elf} {bin}".format(elf=elf, bin=bin_path)
    print("Creating binary:", cmd)
    env.Execute(cmd)
    create_hex(bin_path, env)
    return None

env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", create_bin)
