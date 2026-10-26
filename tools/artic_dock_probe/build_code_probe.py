"""Build the real NCCH codec in a standalone probe using the configured project flags."""
import argparse
from pathlib import Path
import shlex
import subprocess


def run(args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build_directory.resolve()
    target = "src/core/CMakeFiles/citra_core.dir/file_sys/ncch_container.cpp.o"
    commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", target], text=True)
    command = shlex.split(commands.strip().splitlines()[-1])
    flags = []
    skip = False
    for argument in command[1:]:
        if skip:
            skip = False
            continue
        if argument in ("-o", "-MF", "-MT", "-c"):
            skip = True
            continue
        if argument != "-MD":
            flags.append(argument)
    output = build / "codec-probe"
    output.mkdir(exist_ok=True)
    compiler = command[0]
    codec = output / "ncch_codec.o"
    run([compiler, *flags, "-ffunction-sections", "-fdata-sections", "-c",
         root / "src/core/file_sys/ncch_container.cpp", "-o", codec])
    sources = ["src/network/artic_base/title_reader.cpp",
               "src/network/artic_base/artic_base_client.cpp", "src/network/socket_manager.cpp"]
    objects = [codec]
    auxiliary = [*flags, "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
                 "-I" + str(root / "tools/artic_dock_probe/stubs")]
    # Put probe logging headers before the production include paths.
    auxiliary.insert(0, auxiliary.pop())
    for index, source in enumerate(sources):
        obj = output / f"network{index}.o"
        run([compiler, *auxiliary, "-c", root / source, "-o", obj])
        objects.append(obj)
    for name, source, dependencies in (
            ("artic-code-probe", "code_probe.cpp", objects),
            ("test-code-decompress", "test_code_decompress.cpp", [codec])):
        run([compiler, *auxiliary, "-pthread", root / "tools/artic_dock_probe" / source,
             *dependencies, "-Wl,--gc-sections", "-o", output / name])
    run([output / "test-code-decompress"])
    print(output / "artic-code-probe")


if __name__ == "__main__":
    main()
