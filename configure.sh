#!/usr/bin/env bash
set -e
preset="$1"
printf "configuring project with the preset: $preset\n"
cmake --preset "$preset"
printf "Copying compile commands JSON to build so clangd can find them.\n"
cp "build/$preset/compile_commands.json" build/compile_commands.json
