Import("env")

project_dir = env["PROJECT_DIR"]
openocd_cfg = project_dir + "/openocd.cfg"
firmware    = project_dir + "/.pio/build/mini_m4_tiva/firmware.elf"

env.Replace(
    UPLOADCMD='openocd -f "%s" -c "program {%s} verify" -c "reset run" -c "sleep 200" -c "shutdown"'
              % (openocd_cfg, firmware)
)
