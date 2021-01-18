# BM

![birch](./assets/birch-296x328.png)

Simple Virtual Machine with its own Bytecode and Assembly language.

## Quick Start

```console
$ ./build.sh                  # or ./build_msvc.bat if you are on Windows
$ ./build/bin/bme -i ./build/examples/hello.bm
$ ./build/bin/bme -i ./build/examples/fib.bm
$ ./build/bin/bme -i ./build/examples/e.bm
$ ./build/bin/bme -i ./build/examples/pi.bm
```

## Components

### basm

Assembly language for the Virtual Machine. For examples see [./examples/](./examples) folder.

### bme

BM emulator. Used to run programs generated by [basm](#basm).

### bdb

BM debuger. Used to step debug programs generated by [basm](#basm).

### debasm

Disassembler for the binary files generated by [basm](#basm)

## Editor Support

### Emacs

Emacs mode available in [./tools/basm-mode.el](./tools/basm-mode.el). Until the language stabilized and we upload the mode on [MELPA](https://melpa.org/) you need to install this mode manually.

Add the following lines to your `.emacs` file:

```emacs-lisp
(add-to-list 'load-path "/path/to/basm-mode/")
(require 'basm-mode)
```

### Vim

Copy [./tools/basm.vim](./tools/basm.vim) in `.vim/syntax/basm.vim`. Add the following line to your `.vimrc` file:

```vimscript
autocmd BufRead,BufNewFile *.basm set filetype=basm
```
