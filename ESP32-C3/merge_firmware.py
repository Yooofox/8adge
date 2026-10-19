Import("env")
import os

# The merged image is the ONLY artefact worth keeping: it already contains
# bootloader + partition table + app, so it is flashed at offset 0x0 in one go.
# After every build it is copied to this project's own bin/ directory under a
# stable name, so the flashing / release workflow never has to guess which file
# in .pio/build/ is the right one.
PUBLISHED_NAME = "8adge_BLE_398_A1.bin"
MANIFEST_NAME = "SHA256.txt"


def publish_merged_bin(env, merged_path):
    import hashlib
    import shutil

    project_dir = env.subst("$PROJECT_DIR")                 # .../ESP32-C3
    bin_dir = os.path.join(project_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)

    dest = os.path.join(bin_dir, PUBLISHED_NAME)
    shutil.copyfile(merged_path, dest)

    with open(dest, "rb") as f:
        data = f.read()
    sha256 = hashlib.sha256(data).hexdigest().upper()

    # Three-line manifest: name, byte count, SHA256.
    manifest = os.path.join(bin_dir, MANIFEST_NAME)
    with open(manifest, "w", encoding="utf-8", newline="\n") as f:
        f.write("{}\n  {} bytes\n  {}\n".format(PUBLISHED_NAME, len(data), sha256))

    print("Published firmware -> {}".format(dest))
    print("  {} bytes  SHA256 {}".format(len(data), sha256))


def merge_bin(source, target, env):
    firmware = str(target[0])
    board = env.BoardConfig()
    mcu = board.get("build.mcu", "esp32")
    flash_mode = env.GetProjectOption("board_build.flash_mode", "dio")
    flash_freq = board.get("build.f_flash", "40000000L").replace("L", "")
    flash_freq_m = str(int(int(flash_freq) / 1000000)) + "m"
    flash_size = board.get("upload.flash_size", "4MB")

    esptool = os.path.join(
        env.PioPlatform().get_package_dir("tool-esptoolpy") or "",
        "esptool.py",
    )
    output = os.path.join(env.subst("$BUILD_DIR"), "firmware_merged.bin")

    extra = env.get("FLASH_EXTRA_IMAGES", [])
    print("=== FLASH_EXTRA_IMAGES ===")
    for offset, path in extra:
        print(f"  {offset} -> {env.subst(path)}")
    print(f"  APP -> 0x10000 {firmware}")
    print("==========================")

    cmd = [
        "$PYTHONEXE", esptool,
        "--chip", mcu,
        "merge_bin",
        "--flash_mode", flash_mode,
        "--flash_freq", flash_freq_m,
        "--flash_size", flash_size,
        "-o", output,
    ]

    for offset, path in extra:
        cmd.extend([offset, env.subst(path)])

    cmd.extend(["0x10000", firmware])

    env.Execute(env.VerboseAction(" ".join(cmd), "Merging firmware into single binary"))
    publish_merged_bin(env, output)

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin)
