# TAPe UTility

Command line tool for processing TAP files.

## Usage

```
TAPUT command [options] FileIn [FileOut]
Commands:
    add              Add a file at the end of the "tap" image
                     if the image does not exist, it will be created
    export-all       Extract every data block to its own file in the given
                     folder (named after the block header). With --inf, also
                     writes a .inf metadata sidecar next to each file
    export-png       Render a SCREEN$ block to a 256x192 PNG image.
                     With -s <n> the SCREEN$ is read from block n of a
                     "tap" image; with no -s the input is treated as a
                     standalone .scr file
    extract          Extract a block of data to a file
    fix-0            Remove empty blocks from the image
    fix-crc          Correct the checksum of the selected blocks
    info             Show a summary of the image (block counts, integrity)
    insert           Insert a file at the beginning of the image,
                     with the -s <n> option inserts a file before block n
    list             List image content
    remove           Remove blocks of data from the image (requires -s (<n>|<n>-<m>))
                     up to 10 blocks can be selected
    replace          Replace the data block with the contents of the file
Options:
    -b               Creates a BASIC program header block
    --from-inf <f>   Read header name/type/address from a .inf file
                     (use with add / insert / replace)
    -h, --help       Display this help and exit
    --inf            With export-all, also write .inf metadata sidecars
    -n <name>        Implies creating a block header,
                     <name> sets a block name
    -o <addr>        Implies creating a block header,
                     <addr> sets the origin address or the BASIC autostart line number
    -r, --raw        Treats input/output files as raw data,
                     refrains from creating a flag and checksum
    -s (<n>|<n>-<m>)[,(<n>|<n>-<m>)]..
                     Selects the block numbers and/or block ranges (up to 10)
Examples:
    taput add -o 32768 -n "block name" file.bin image.tap
    taput export-all --inf image.tap ./out
    taput export-png -s 2 image.tap screen.png
    taput export-png screen.scr screen.png
    taput extract -s 2 image.tap file.bin
    taput fix-0 image.tap
    taput fix-crc -s 3 image.tap
    taput info image.tap
    taput insert -s 3 -o 32768 -n "block name" file.bin image.tap
    taput list image.tap
    taput remove -s 1-4,8,12 image.tap
    taput replace -s 2 file.bin image.tap
```

## `.inf` metadata round-trip

`export-all --inf` writes a small text sidecar (`<file>.inf`) capturing the
block header, so an extracted file can be re-added with its name, type and
address intact:

```
taput export-all --inf game.tap ./out         # out/02_loader.bin (+ .inf)
taput add --from-inf out/02_loader.bin.inf out/02_loader.bin rebuilt.tap
```

The rebuilt image is byte-identical to the original block. A SCREEN$ block
(CODE at 16384, 6912 bytes) is detected automatically and exported with a
`.scr` extension, ready for `export-png`.

## Building

```
make            # native build (auto-detects macOS / Linux / Windows)
make mac        # macOS universal binary (arm64 + x86_64)
make linux      # Linux x86_64, static (musl)
make windows    # Windows x86_64, static (mingw)
make release    # all three
make test       # build, then run the regression suite
```

Cross-compiling from macOS needs the toolchains:

```
brew install FiloSottile/musl-cross/musl-cross   # Linux target
brew install mingw-w64                            # Windows target
```
