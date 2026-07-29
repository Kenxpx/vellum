# The qasm text assembly format

`qasm` is Vellum's small line-oriented assembler. It reads a text source and
produces a `.qbc` module (see [BYTECODE.md](BYTECODE.md) for the binary format
it emits). The grammar is defined by `src/asm.h` and implemented against the
module builder; it is a convenience tool and is not on the fuzzing path.

The assembler is deliberately simple: **one item per line**. A line is either a
directive, a label, an instruction, a comment, or blank. Leading and trailing
whitespace is ignored, and blank lines are skipped.

## Comments

A `#` begins a comment that runs to the end of the line. A comment may sit on a
line by itself or follow any directive, label, or instruction. Everything from
the `#` onward is discarded before the line is parsed.

```
# this whole line is a comment
pushint 7      # and so is this trailing note
```

## Directives

Directives begin with a `.` and configure the module and its functions. They
mirror the fields of the binary format one-for-one.

### `.const`

Adds an entry to the module's constant pool. There is one form per constant
kind:

```
.const int <n>          # a 64-bit signed integer
.const real <x>         # a 64-bit floating-point number
.const string "<text>"  # a length-prefixed byte string
```

Constants are numbered in the order they are declared, starting at 0. That index
is what `pushk` refers to. String literals are wrapped in double quotes and use
the usual backslash escapes (`\"`, `\\`, `\n`, `\t`, `\r`).

### `.func`

Begins a new function body:

```
.func <arity> <locals>
```

`arity` is the number of arguments the function takes and `locals` is the total
number of local slots it uses, **including** those arguments (so `locals` is
always at least `arity`). Every instruction and label that follows belongs to
this function until the next `.func`. Functions are numbered in declaration
order from 0; that index is what `closure` and `.entry` refer to.

### `.upval`

Declares one captured upvalue for the function currently being assembled. It
must appear after the function's `.func` line and before its instructions:

```
.upval local <i>    # capture slot <i> of the enclosing frame
.upval upval <i>    # re-capture upvalue <i> of the enclosing function
```

Upvalues are numbered in declaration order and are what `getupval` / `setupval`
address.

### `.entry`

Selects which function is the module's entry point:

```
.entry <func-index>
```

If omitted, the entry defaults to function 0. There should be exactly one
`.entry` per module.

## Labels

A label is an identifier followed by a colon, alone on its line:

```
loop:
```

A label marks the position of the next instruction within the current function
and can be used as the operand of a jump (`jmp`, `jmpif`, `jmpifnot`). Labels
are function-local: a jump may only target a label in the same function, and the
assembler resolves each label to the correct little-endian `i16` relative offset
(measured from the byte after the operand). Labels may be referenced before they
are defined (forward jumps).

## Instructions

Every other non-blank line is a single instruction: a lowercase mnemonic
optionally followed by one operand.

```
<mnemonic> [operand]
```

The mnemonic is the opcode name without the `OP_` prefix, lowercased -- for
example `pushint`, `loadlocal`, `add`, `jmpifnot`, `call`, `ret`. An
instruction takes an operand exactly when its opcode does (see the operand and
stack-effect table in [BYTECODE.md](BYTECODE.md)); opcodes with no operand take
none. Operand kinds are:

- **immediate integer** -- `pushint 7`.
- **pool / slot / index** -- a small unsigned number: `pushk 0`, `loadlocal 1`,
  `storelocal 2`, `getupval 0`, `call 1`, `newarray 3`.
- **label** -- for the jumps: `jmp loop`, `jmpifnot done`.

## Example: an arithmetic function

A single entry function that computes `(3 + 4) * 2`, prints it, and halts.

```
# multiply the sum of two ints by a third
.func 0 0
    pushint 3
    pushint 4
    add             # -> 7 on the stack
    pushint 2
    mul             # -> 14
    print
    halt
.entry 0
```

## Example: a counting loop with a label

Counts from 0 up to (but not including) 5, printing each value. Local slot 0
holds the counter. The `loop` label is the jump target and `done` marks the
exit.

```
# print 0,1,2,3,4
.const int 5
.func 0 1
    pushint 0
    storelocal 0            # i = 0
loop:
    loadlocal 0
    pushk 0                 # push the constant 5
    lt                      # i < 5 ?
    jmpifnot done           # if not, fall out of the loop
    loadlocal 0
    print                   # print i
    loadlocal 0
    pushint 1
    add
    storelocal 0            # i = i + 1
    jmp loop
done:
    pushnil
    ret
.entry 0
```

Here `jmp loop` is a backward jump to an already-seen label and `jmpifnot done`
is a forward jump the assembler patches once `done:` is reached. Both resolve to
relative `i16` offsets in the emitted bytecode.
