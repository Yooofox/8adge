Import("env")
import os

# 合并镜像（merged image）是唯一值得保留的产物：它已经包含 bootloader + 分区表
# + app，所以只需在偏移 0x0 处一次烧录即可。每次构建完成后，它都会以固定的
# 文件名被复制到本项目自己的 bin/ 目录下，因此烧录 / 发布流程
# 永远不必去猜 .pio/build/ 里哪一个文件才是正确的那个。
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

    # 三行清单：文件名、字节数、SHA256。
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
