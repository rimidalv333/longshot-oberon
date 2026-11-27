Longshot Oberon - the Windows builder (no WSL)
==============================================

What is in this folder
  build.cmd                               the builder: put your oot.nup in this folder and double-click it (or drag the
                                          .nup onto it from anywhere)
  longshot-oberon-2.0.0-20261127.zip      the release (source, PS5 libraries, the shipped renderer profile vk.profdata)
  tools\llvm                              LLVM/Clang 18.1.8 for Windows (official release; only the tools the build uses), plus the
                                          Linux builtins library the PS5 link takes from the Linux release (target code, the same file)
  tools\python                            CPython 3.12.8 for Windows (python-build-standalone 20250115) with the build's packages
                                          (lz4, numpy, capstone, unicorn, zstandard, fonttools, pillow, astc-encoder-py, archspec,
                                          texture2ddecoder: the same versions as the Linux deps/python), and sitecustomize.py, which
                                          makes Python write text files with LF line endings, as on Linux
  tools\bin                               a64emit.dll (the lifter's instruction translator), zbic.dll (the .nup reader's zstd
                                          variant) and ps5-native-tool.exe (ELF -> the console's SELF), built for Windows from the
                                          same sources the Linux tools are built from (llvm-mingw, static: nothing to install)
  build_args.txt                          options for a double-click run (here: --jobs 3)

How to build
  1. Copy your oot.nup into this folder, beside build.cmd (no path to set anywhere). Or drag the .nup onto build.cmd.
  2. Double-click build.cmd. The first run unpacks the zip into longshot_oberon\ beside it, then: checks the .nup, lifts the game's
     program to C, makes the shader list and button prompts, compiles and links the title. About an hour the first time.
  3. The title folder is longshot_oberon\dist\PPSA93339. The end of the window and of build.log says where to put it and
     the game file on the console (oot.nup can sit inside the PPSA93339 folder itself), and eboot.bin's sha256.
  A step that is done is not done again: run build.cmd again after an interruption. build.log keeps every run's output.

What it runs
  ps5\win_build.py: build.sh's steps in Python (the same checks, marks, logs and title folder).
  ps5\build.py: the compile, unchanged but for the compiler wrapper (prospero-clang18's flags, given directly) on Windows.
  ps5\tools\link.py: link.sh and PS5_Vulkan's radv-link.sh in Python, step for step and argument for argument. Run on Linux
  against the same objects it links a byte-identical ELF to link.sh's (checked 2026-11-27: sha256 1102e0ef...).
  The settings the release is built with (the same as the Linux command line RX_FP_NATIVE=1 PS5_PGO=ps5/profiles/vk.profdata)
  are set in build.cmd.

The same eboot.bin as Linux/WSL
  From the same release zip and the same .nup, the Windows build and a Linux/WSL build of a fresh unpack make the same eboot.bin:
  - the lifted C is the same (checked file by file against a WSL lift; a64_stubs.c's comments no longer depend on whether
    llvm-cxxfilt is installed);
  - an object compiled by the Windows clang is the same bytes as the Linux clang's (checked: game_code_011.o);
  - __FILE__ names files from the tree's root in '/' form (-ffile-prefix-map, -ffile-reproducible), not the folder the zip was
    unpacked to.
  A build in a tree that has lifted before can split the hot code differently (nx2/split_hot.py keeps the last lift's pieces), so
  compare fresh unpacks, not an old working tree.
