import os

Import("env")

toolchain_dir = env.PioPlatform().get_package_dir("toolchain-gccmingw32")
if not toolchain_dir:
    raise RuntimeError("PlatformIO MinGW toolchain is not installed")

bin_dir = os.path.join(toolchain_dir, "bin")
os.environ["PATH"] = bin_dir + os.pathsep + os.environ.get("PATH", "")
env.PrependENVPath("PATH", bin_dir)
env.Replace(
    AR=os.path.join(bin_dir, "ar.exe"),
    CC=os.path.join(bin_dir, "gcc.exe"),
    CXX=os.path.join(bin_dir, "g++.exe"),
    RANLIB=os.path.join(bin_dir, "ranlib.exe"),
)
env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
