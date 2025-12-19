# Porkchop pre-build script
# Ensures model files exist and generates version info

Import("env")
import os
import subprocess
from datetime import datetime

def get_git_commit():
    """Get short git commit hash, or 'unknown' if not in a git repo"""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return "unknown"

def pre_build_callback(source, target, env):
    """Generate build info header"""
    build_info = {
        "build_time": datetime.now().isoformat(),
        "version": env.GetProjectOption("custom_version", "0.1.1"),
        "commit": get_git_commit()
    }
    
    info_path = os.path.join(env.get("PROJECT_SRC_DIR"), "build_info.h")
    with open(info_path, "w") as f:
        f.write("// Auto-generated build info\n")
        f.write("#pragma once\n")
        f.write(f'#define BUILD_TIME "{build_info["build_time"]}"\n')
        f.write(f'#define BUILD_VERSION "{build_info["version"]}"\n')
        f.write(f'#define BUILD_COMMIT "{build_info["commit"]}"\n')

env.AddPreAction("buildprog", pre_build_callback)
