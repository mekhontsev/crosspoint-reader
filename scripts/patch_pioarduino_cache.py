import hashlib
import shutil
import sys
from pathlib import Path


def cache_matches(requested, cached_request):
    return bool(requested.strip()) and requested.strip() == cached_request.strip()


if "--self-test" in sys.argv:
    assert cache_matches("CONFIG_A=y\nCONFIG_B=n", "CONFIG_A=y\nCONFIG_B=n")
    assert not cache_matches("CONFIG_A=y", "CONFIG_A=y\nCONFIG_B=n")
    raise SystemExit(0)


Import("env")  # noqa: F821 -- provided by PlatformIO

platform = env.PioPlatform()
board = env.BoardConfig()
mcu = board.get("build.mcu", "esp32")
chip_variant = board.get("build.chip_variant", "").lower() or mcu
framework_libs = Path(platform.get_package_dir("framework-arduinoespressif32-libs"))

# PioArduino's component manager edits this generated file in place when a
# normal CrossPoint profile excludes the Arduino BLE library. Every X4 Pro
# plugin-host profile needs the underlying ESP-IDF Bluetooth component, so
# restore the package-provided full template before the framework builder reads
# it.
if env.subst("$PIOENV") in {
    "x4pro",
    "x4pro-gh_release",
    "x4pro-gh_release_rc",
}:
    component_build = framework_libs / chip_variant / "pioarduino-build.py"
    component_template = framework_libs / chip_variant / f"pioarduino-build.py.{chip_variant}"
    if component_template.is_file() and (
        not component_build.is_file()
        or component_build.read_bytes() != component_template.read_bytes()
    ):
        shutil.copyfile(component_template, component_build)
        print(f"Restored Bluetooth-capable Arduino framework for {chip_variant}")

builder = Path(platform.get_dir()) / "builder" / "frameworks" / "arduino.py"
source = builder.read_text(encoding="utf-8")
# The package already stores libraries per chip; only its cache-presence check is global.
old_check = '''flag_any_custom_sdkconfig = (FRAMEWORK_LIB_DIR is not None and 
                            exists(str(Path(FRAMEWORK_LIB_DIR) / "sdkconfig")))'''
new_check = '''flag_any_custom_sdkconfig = (
    FRAMEWORK_LIB_DIR is not None
    and exists(str(Path(FRAMEWORK_LIB_DIR) / chip_variant / "sdkconfig.orig"))
)'''

if old_check in source:
    builder.write_text(source.replace(old_check, new_check, 1), encoding="utf-8")
elif new_check not in source:
    raise RuntimeError("Unsupported pioarduino cache check")

requested = env.GetProjectOption("custom_sdkconfig", "")
target_sdkconfig = framework_libs / chip_variant / "sdkconfig"
original_sdkconfig = framework_libs / chip_variant / "sdkconfig.orig"
request_file = framework_libs / chip_variant / "sdkconfig.crosspoint"

if original_sdkconfig.is_file():
    cached_request = (
        request_file.read_text(encoding="utf-8") if request_file.is_file() else ""
    )
    if cache_matches(requested, cached_request):
        marker = "# TASMOTA__" + hashlib.md5(
            (requested.strip() + mcu).encode("utf-8")
        ).hexdigest()[:16]
        defaults = Path(env.subst("$PROJECT_DIR")) / "sdkconfig.defaults"
        lines = (
            defaults.read_text(encoding="utf-8").splitlines()
            if defaults.exists()
            else []
        )
        if not lines or lines[0] != marker:
            defaults.write_text(
                "\n".join([marker, *lines[1:]]) + "\n", encoding="utf-8"
            )
            print(f"Restored cached Arduino framework for {mcu}")
    else:
        # Recompile only this chip from its original template after config changes.
        original_sdkconfig.replace(target_sdkconfig)

request_file.write_text(requested.strip() + "\n", encoding="utf-8")
