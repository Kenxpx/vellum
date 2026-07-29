# The Vellum source language

Vellum is a small, statically-parsed scripting language that compiles to Vellum
bytecode modules (`.qbc`). This document describes the surface language that the
front end accepts: the lexer (`src/lexer.h`), the recursive-descent parser
(`src/parser.h`, `src/ast.h`), and the compiler (`src/compiler.h`) that lowers a
parsed program to a module. For the byte-level details of what the compiler
emits, see [BYTECODE.md](BYTECODE.md).

A program is compiled to a `.qbc` file with the `qc` compiler and executed with
the `qrun` runner:

```
qc program.ql -o program.qbc
qrun program.qbc
```

`qrun` runs the module's entry function and prints the resulting value to
standard output as `=> <value>`.

## Lexical structure

Source text is read as a stream of tokens. Whitespace (spaces, tabs, newlines)
separates tokens but is otherwise insignificant. A `#` begins a line comment:
everything from the `#` to the end of the line is ignored.

```
# this is a comment
let x = 1;   # trailing comments are fine too
```

The lexer recognizes:

- **Integer literals** such as `0`, `42`, `1000` (a 64-bit signed value).
- **Real literals** such as `3.14` and `0.5` (a 64-bit floating-point value).
- **String literals** in double quotes, such as `"hello"`.
- **Identifiers**, which name variables and functions.
- **Keywords**: `fn`, `let`, `if`, `else`, `while`, `return`, `print`, `true`,
  `false`, `nil`, `and`, `or`. Keywords are reserved and cannot be used as
  identifiers.
- **Operators and punctuation**: `+ - * / %`, the comparisons `== != < <= > >=`,
  assignment `=`, logical `!`, and the delimiters `( ) { } [ ] , ; :`.

## Program structure

A program is a sequence of one or more function definitions. There are no
top-level statements: all executable code lives inside a function.

```
fn name(param1, param2) {
    ... statements ...
}
```

A function has a name, a comma-separated (possibly empty) parameter list, and a
body enclosed in braces. Parameters become the function's first local variables.

Execution begins at the function named **`main`**, which is the module entry and
must take no parameters. Its return value is what `qrun` prints.

Functions may call one another by name; a call resolves to the target
function's index at compile time. A call to an unknown name is a compile error.

## Statements

The body of a function, and the body of any `if`/`while`, is a block: a
brace-enclosed sequence of statements. The language provides the following
statement forms.

### Variable binding: `let`

```
let count = 0;
let message = "done";
```

`let` introduces a new local variable, initialized to the value of the
expression on the right. Each binding occupies a fresh local slot in the current
function.

### Assignment

```
count = count + 1;
grid[i] = value;
```

An assignment updates an existing target with a new value. The target is either
an identifier (a previously bound variable) or an index expression `base[key]`
(an element of an array or a map).

### Conditionals: `if` / `else`

```
if x < 0 {
    print "negative";
} else {
    print "non-negative";
}
```

The condition is an expression; the `then` and optional `else` branches are
blocks. The `else` clause may be omitted. Branches lower to conditional jumps
with backpatched offsets.

### Loops: `while`

```
while i < n {
    total = total + i;
    i = i + 1;
}
```

A `while` loop repeatedly evaluates its condition and runs its body block while
the condition is true.

### `return`

```
return result;
return;          # returns nil
```

`return` ends the current function. With an expression, it yields that value;
with none, it yields `nil`.

### `print`

```
print value;
```

`print` evaluates its expression and emits the value.

### Expression statements

An expression followed by `;` is a statement, evaluated for its effect. This is
most commonly a function call:

```
do_work(x, y);
```

## Expressions

Vellum expressions follow the usual precedence, from lowest binding to highest:

| Level          | Operators                | Associativity |
|----------------|--------------------------|---------------|
| logical or     | `or`                     | left          |
| logical and    | `and`                    | left          |
| comparison     | `== != < <= > >=`        | left          |
| additive       | `+ -`                    | left          |
| multiplicative | `* / %`                  | left          |
| unary          | `- !` (prefix)           | right         |
| call / index   | `f(...)`  `base[...]`     | left          |
| primary        | literals, identifiers,   |               |
|                | `( ... )`, `[...]`, `{...}` |             |

Parentheses `( ... )` group a subexpression and override precedence.

### Operators

The arithmetic operators `+`, `-`, `*`, `/`, and `%` combine numeric operands.
The comparison operators `==`, `!=`, `<`, `<=`, `>`, `>=` yield a boolean. The
logical `and` and `or` combine boolean operands, and prefix `!` negates a
boolean. Prefix `-` negates a number.

### Literals

- **`nil`** - the absence of a value.
- **`true`** and **`false`** - the boolean values.
- **Integers** and **reals** - as described in the lexical section.
- **Strings** - double-quoted text.

### Identifiers

An identifier evaluates to the value of the variable it names (a parameter or a
`let` binding). Referring to an unbound name is a compile error.

### Array literals

An array literal is a comma-separated list of expressions in square brackets:

```
let xs = [1, 2, 3];
let empty = [];
```

Elements are read and written by integer index with `xs[i]`.

### Map literals

A map literal is a comma-separated list of `key: value` pairs in braces:

```
let ages = {"ada": 36, "alan": 41};
```

Entries are read and written by key with `ages["ada"]`.

### Calls and indexing

A call applies a named function to zero or more argument expressions:

```
let r = combine(a, b, 3);
```

Indexing `base[key]` reads an element of an array (by integer) or a map (by
key). The same form on the left of an `=` writes that element.

## Example: iterative factorial

This program computes a factorial with a `while` loop rather than recursion, and
returns the result from `main`.

```
# factorial of n, computed iteratively
fn factorial(n) {
    let result = 1;
    let i = 2;
    while i <= n {
        result = result * i;
        i = i + 1;
    }
    return result;
}

fn main() {
    return factorial(5);   # => 120
}
```

## Example: working with an array

This program builds an array, sums its elements in a loop, and prints the total.

```
fn sum(xs, n) {
    let total = 0;
    let i = 0;
    while i < n {
        total = total + xs[i];
        i = i + 1;
    }
    return total;
}

fn main() {
    let values = [10, 20, 30, 40];
    print sum(values, 4);   # prints 100
    return 0;
}
```

## Errors

The parser reports syntax errors as `line N: ...` messages. The compiler reports
semantic errors - such as an unknown name, too many locals, or a missing `main`
function - in the same form. In both cases compilation stops and no module is
produced.
