# PlatformIO pre-build script: gzip the web interface sources in data_src/
# into data/, which is what buildfs/uploadfs pack into the LittleFS image.
# Runs on every build, so data/ can never go stale.
import gzip
import os

Import("env")  # noqa: F821 (provided by PlatformIO/SCons)

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
src_dir = os.path.join(project_dir, "data_src")
out_dir = os.path.join(project_dir, "data")
os.makedirs(out_dir, exist_ok=True)

expected = set()
for name in sorted(os.listdir(src_dir)):
    src = os.path.join(src_dir, name)
    if not os.path.isfile(src):
        continue
    dst = os.path.join(out_dir, name + ".gz")
    expected.add(name + ".gz")
    with open(src, "rb") as f:
        data = f.read()
    # mtime=0 keeps the output byte-identical between builds
    packed = gzip.compress(data, compresslevel=9, mtime=0)
    old = None
    if os.path.exists(dst):
        with open(dst, "rb") as f:
            old = f.read()
    if old != packed:
        with open(dst, "wb") as f:
            f.write(packed)
        print("gzip_web: %s -> data/%s.gz (%d bytes)" % (name, name, len(packed)))

# Remove files that no longer have a source (e.g. a deleted page)
for name in os.listdir(out_dir):
    if name not in expected:
        os.remove(os.path.join(out_dir, name))
        print("gzip_web: removed stale data/%s" % name)
