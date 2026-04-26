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

**PostgreSQL (optional):** If `libpq` is available (`pkg-config libpq`, or a typical Homebrew install at `/opt/homebrew/opt/libpq` or `/usr/local/opt/libpq`), the interpreter is linked against it and exposes `pg_connect`, `pg_close`, and `pg_query`. Without `libpq`, those builtins still exist but return an error at runtime (“PostgreSQL support was not compiled in”). To force the stub even when `libpq` is present, build with `FORCE_POSTGRES_STUB=1 make`.

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

**Format a file (stdout):**

```sh
bowie format script.bow
```

**Format in place:**

```sh
bowie format --write script.bow
```

Formatter behavior can be configured in `bowie.json`:

```json
{
  "format": {
    "indentStyle": "spaces",
    "indentSize": 4,
    "maxConsecutiveBlankLines": 1,
    "spaceBeforeInlineComment": 2,
    "finalNewline": true
  }
}
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

### PostgreSQL

When built with **libpq**, Bowie can talk to PostgreSQL using connection strings in the same form as `psql` / `PQconnectdb` (for example `postgresql://user:pass@localhost:5432/dbname` or `host=localhost port=5432 dbname=mydb`).

| Builtin       | Description |
| ------------- | ----------- |
| `pg_connect(conninfo)` | Opens a connection; returns a `pg_conn` value or an error. |
| `pg_close(conn)`       | Closes the connection. |
| `pg_query(conn, sql)`  | Runs `sql`. For **rows** (`SELECT`, etc.), returns an **array of hashes** (column names → string or `null`). For commands without result rows (`INSERT`, `UPDATE`, …), returns a hash with `command` (status string) and `rows` (affected row count when available). On failure, returns an **error** value; use `type(x) == "error"` (or propagate) like other builtins. |

```bowie
let db = pg_connect("host=localhost dbname=test")
if type(db) == "error" {
  println(str(db))
} else {
  let rows = pg_query(db, "SELECT id::text AS id, name FROM users LIMIT 10")
  pg_close(db)
}
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
| `src/postgres.c`      | PostgreSQL client (`libpq`), when enabled |
| `src/postgres_disabled.c` | Stub `pg_*` builtins when `libpq` is absent |
| `src/main.c`          | Entry point (REPL + file runner)   |

## Examples

See the [examples](https://github.com/bowie-lang/examples) for runnable programs.
