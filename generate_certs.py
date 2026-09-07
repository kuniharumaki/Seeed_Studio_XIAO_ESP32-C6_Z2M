Import("env")
import os
import shutil
import subprocess

build_dir = env.subst("$BUILD_DIR")
env_name = os.path.basename(build_dir)
nested_dir = os.path.join(build_dir, ".pio", "build", env_name)
os.makedirs(nested_dir, exist_ok=True)

user_home = os.path.expanduser("~")
cmake_exe = os.path.join(user_home, ".platformio", "packages", "tool-cmake", "bin", "cmake.exe")
data_file_asm = os.path.join(user_home, ".platformio", "packages", "framework-espidf", "tools", "cmake", "scripts", "data_file_embed_asm.cmake")

certs = [
    ("managed_components/espressif__esp_insights/server_certs/https_server.crt", "https_server"),
    ("managed_components/espressif__esp_rainmaker/server_certs/rmaker_claim_service_server.crt", "rmaker_claim_service_server"),
    ("managed_components/espressif__esp_rainmaker/server_certs/rmaker_mqtt_server.crt", "rmaker_mqtt_server"),
    ("managed_components/espressif__esp_rainmaker/server_certs/rmaker_ota_server.crt", "rmaker_ota_server"),
]

for src_rel, name in certs:
    src_path = os.path.abspath(src_rel)
    if os.path.exists(src_path):
        out1 = os.path.join(build_dir, f"{name}.crt.S")
        out2 = os.path.join(nested_dir, f"{name}.crt.S")
        subprocess.run(
            [cmake_exe, f"-DDATA_FILE={src_path}", f"-DSOURCE_FILE={out1}", "-DFILE_TYPE=TEXT", "-P", data_file_asm],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if os.path.exists(out1) and os.path.abspath(out1) != os.path.abspath(out2):
            shutil.copy2(out1, out2)
