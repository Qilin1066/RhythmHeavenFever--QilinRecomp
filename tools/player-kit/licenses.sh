# The licences every download carries (sourced by package.sh,
# package-linux.sh and package-macos.sh, which add their compiler's or
# MoltenVK's), as Punch-Out's: project_licenses <LICENSES folder>, with
# $root (the source), $build (the build folder) and $PACKAGE set; crlf=1
# gives the project's own texts Windows line ends.
#   <PACKAGE>/LICENSE.txt      this project (GPL-3.0)
#   rhf-decomp/                third_party/rhf (CC0 1.0): kit/rhf is its
#                              symbols.txt and splits.txt, unchanged
#   SDL3/, Dear ImGui/, Vulkan-Headers/, volk/
#                              what the launcher and the game's runtime are
#                              built with (volk: the launcher only)
project_licenses() {
    lic=$1
    to_text() { if [ "${crlf:-0}" = 1 ]; then sed 's/$/\r/' "$1"; else cat "$1"; fi; }
    mkdir -p "$lic/$PACKAGE" "$lic/rhf-decomp" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk"
    to_text "$root/LICENSE" > "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0
    to_text "$root/third_party/rhf/LICENSE" > "$lic/rhf-decomp/LICENSE.txt"  # CC0 1.0
    {
        echo "kit/rhf/symbols.txt and kit/rhf/splits.txt are the symbols and splits of the"
        echo "Rhythm Heaven Fever decompilation (https://github.com/conhlee/rhf, commit"
        echo "02b140c9e7007e2a4ddd61c80ac3f2eb35251ed7), unchanged, under CC0 1.0: LICENSE.txt"
        echo "here. The recompiler ports their US names to your own disc's main.dol when it"
        echo "builds the game (work/pal_symbols.txt)."
    } > "$lic/rhf-decomp/README.txt.tmp"
    to_text "$lic/rhf-decomp/README.txt.tmp" > "$lic/rhf-decomp/README.txt"
    rm "$lic/rhf-decomp/README.txt.tmp"
    cp "$build/_deps/sdl3-src/LICENSE.txt" "$lic/SDL3/"
    imgui=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=//p' "$build/CMakeCache.txt")
    [ -n "$imgui" ] || imgui="$build/_deps/imgui-src"
    cp "$imgui/LICENSE.txt" "$lic/Dear ImGui/"
    cp "$build/_deps/vulkanheaders-src/LICENSE.md" "$lic/Vulkan-Headers/" 2>/dev/null || cp "$build/_deps/vulkanheaders-src/LICENSE"* "$lic/Vulkan-Headers/"
    cp "$build/_deps/volk-src/LICENSE.md" "$lic/volk/"
    for f in "$lic/$PACKAGE/LICENSE.txt" "$lic/rhf-decomp/LICENSE.txt" "$lic/SDL3/LICENSE.txt" "$lic/Dear ImGui/LICENSE.txt" "$lic/volk/LICENSE.md"; do
        [ -s "$f" ] || { echo "no licence text: $f" >&2; exit 1; }
    done
    [ -n "$(ls "$lic/Vulkan-Headers")" ] || { echo "no licence text: Vulkan-Headers" >&2; exit 1; }
}
