# Bowie

Bowie is a dynamically typed, interpreted scripting language written in C. Source files use the `.bow` extension.

## Building

Requires a C11 compiler and `make`.

```sh
make
```

The `bowie` binary is produced in the project root.

To install it to `/usr/local/bin`:

```sh
sudo make install
```

To clean build artifacts:

```sh
make clean
```

## Usage

**Run a script:**

```sh
bowie script.bow
```

**Read from stdin:**

```sh
echo 'println("hello")' | bowie -
```

**Start the REPL:**

```sh
bowie
>> println("Hello!")
Hello!
>> exit()
```

## Language

### Types

| Type     | Example                  |
| -------- | ------------------------ |
| `int`    | `42`, `-7`               |
| `float`  | `3.14`, `-0.5`           |
| `string` | `"hello"`                |
| `bool`   | `true`, `false`          |
| `null`   | `null`                   |
| `array`  | `[1, 2, 3]`              |
| `hash`   | `{"key": "value"}`       |
| `fn`     | `fn(x) { return x * 2 }` |

### Variables

```bowie
let name = "Bowie"
let count = 0
count = count + 1
```

### Functions

```bowie
fn add(a, b) {
    return a + b
}

# Anonymous / first-class
let double = fn(x) { return x * 2 }
```

### Control flow

```bowie
if x > 0 {
    println("positive")
} else if x == 0 {
    println("zero")
} else {
    println("negative")
}
```

### Loops

```bowie
while condition {
    # ...
}

for item in collection {
    # ...
}

for i in range(10) {
    # ...
}
```

### Modules

```bowie
# Export from a module
export let PI = 3.14159
export fn square(x) { return x * x }

# Import as namespace
import "math_utils.bow" as math
println(math.PI)

# Import specific names
import "math_utils.bow" use square

# Import everything into scope
import "math_utils.bow"
```

### Comments

```bowie
# This is a comment
```

## Source layout

| File                  | Role                               |
| --------------------- | ---------------------------------- |
| `src/lexer.c/h`       | Tokeniser                          |
| `src/parser.c/h`      | Recursive-descent parser → AST     |
| `src/ast.c/h`         | AST node definitions               |
| `src/interpreter.c/h` | Tree-walking evaluator             |
| `src/object.c/h`      | Value types and reference counting |
| `src/env.c/h`         | Variable scopes                    |
| `src/builtins.c/h`    | Built-in functions                 |
| `src/http.c/h`        | HTTP server support                |
| `src/main.c`          | Entry point (REPL + file runner)   |

## Examples

See the [examples](https://github.com/bowie-lang/examples) for runnable programs.
