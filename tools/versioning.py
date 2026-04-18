Import("env")

from pathlib import Path
import shutil


firmware_version = env.GetProjectOption("custom_firmware_version", "0.0.0").strip()
github_repo = env.GetProjectOption("custom_github_repo", "MrRegata/SnapFan").strip()

env.Append(
    CPPDEFINES=[
        ("APP_VERSION", env.StringifyMacro(firmware_version)),
        ("APP_GITHUB_REPO", env.StringifyMacro(github_repo)),
    ]
)


def after_build(source, target, env):
    source_path = Path(str(source[0]))
    versioned_name = f"snapfan-esp32c3-v{firmware_version}.bin"
    versioned_path = source_path.with_name(versioned_name)
    shutil.copy2(source_path, versioned_path)
    print(f"Versioned OTA binary: {versioned_path}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)