"""Read the firmware sketch as C++ tokens, for source-level characterization.

The firmware as a whole cannot be compiled on the host until the modularization
in docs/BACKLOG.md has happened, so most of its rules can only be checked by
reading the source. The exception is a pure function: definition() and
type_definition() hand over its exact source, and
tests/test_autonomous_schedule.py compiles and runs three of them (D-046).
This module does the reading in one place, with two properties that plain
string searches do not have:

  LOCATION-INDEPENDENT  Every file Arduino compiles is read: the sketch
                        directory's .ino, .h, .c and .cpp files, and src/
                        recursively. A function that moves out of
                        solar-logger.ino into a module is still found. A
                        characterization test that breaks when code merely
                        moves would fail on exactly the change it exists to
                        check.

  FORMAT-INDEPENDENT    Source is compared as a token sequence with comments
                        removed, so indentation, brace placement, line wrapping
                        and comment edits change nothing. A string literal is
                        one token, so a brace or keyword inside a message can
                        never be mistaken for code.

It is NOT a C++ parser. It knows comments, string and character literals,
identifiers, numbers and punctuation, and it finds function bodies by brace
matching. Anything it cannot handle - a raw string literal, an unterminated
literal or comment, unbalanced brackets, two definitions of one function -
raises instead of guessing.
"""

from __future__ import annotations

import functools
import re
from dataclasses import dataclass
from pathlib import Path

from conftest import PROJECT_ROOT

SKETCH_DIR = PROJECT_ROOT / "Arduino" / "solar-logger"

SOURCE_SUFFIXES = frozenset({".ino", ".h", ".hpp", ".c", ".cpp"})

_TOKEN = re.compile(
    r"""
      (?P<space>\s+)
    | (?P<comment>//[^\n]*|/\*.*?\*/)
    | (?P<string>"(?:\\.|[^"\\\n])*")
    | (?P<char>'(?:\\.|[^'\\\n])+')
    | (?P<number>\.?\d(?:[eEpP][+-]|[\w.]|'(?=\w))*)
    | (?P<ident>[A-Za-z_]\w*)
    | (?P<punct>->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=
               |::|\.\.\.|[^\s\w])
    """,
    re.VERBOSE | re.DOTALL,
)

_RAW_STRING_PREFIXES = frozenset({"R", "LR", "uR", "UR", "u8R"})

_OPENERS = {"(": ")", "{": "}", "[": "]"}

# Qualifiers that may sit between a function's parameter list and its body.
_BODY_QUALIFIERS = frozenset({"const", "noexcept", "override", "final"})


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    where: str


def tokenize(text: str, origin: str = "<snippet>") -> list[Token]:
    """Split C++ source into tokens, dropping whitespace and comments."""

    tokens: list[Token] = []
    position = 0
    line = 1

    while position < len(text):
        match = _TOKEN.match(text, position)

        if match is None:  # pragma: no cover - the punct class matches anything
            raise ValueError(f"{origin}:{line}: cannot tokenize {text[position]!r}")

        kind = match.lastgroup or ""
        value = match.group()
        where = f"{origin}:{line}"

        if kind == "punct" and value in ('"', "'"):
            raise ValueError(f"{where}: unterminated {value} literal")

        if kind == "punct" and value == "/" and text.startswith("/*", position):
            raise ValueError(f"{where}: unterminated block comment")

        if (
            kind == "string"
            and tokens
            and tokens[-1].text in _RAW_STRING_PREFIXES
            and position > 0
            and not text[position - 1].isspace()
        ):
            raise ValueError(
                f"{where}: raw string literals are not supported by this reader"
            )

        if kind not in ("space", "comment"):
            tokens.append(Token(kind, value, where))

        line += value.count("\n")
        position = match.end()

    return tokens


def words(snippet: str) -> list[str]:
    """The token texts of a snippet, so patterns can be written as plain C++."""

    return [token.text for token in tokenize(snippet)]


def unquote(token_text: str) -> str:
    """The contents of a simple string literal token."""

    if not (token_text.startswith('"') and token_text.endswith('"')):
        raise ValueError(f"not a string literal: {token_text}")

    inner = token_text[1:-1]

    if "\\" in inner:
        raise ValueError(f"escape sequences are not decoded here: {token_text}")

    return inner


def _closing(texts: list[str], open_index: int) -> int:
    """Index of the bracket that closes the one at `open_index`."""

    opener = texts[open_index]
    closer = _OPENERS[opener]
    depth = 0

    for index in range(open_index, len(texts)):
        if texts[index] == opener:
            depth += 1
        elif texts[index] == closer:
            depth -= 1

            if depth == 0:
                return index

    raise ValueError(f"unbalanced {opener!r} starting at token {open_index}")


class Code:
    """A token sequence plus the handful of searches the tests need."""

    def __init__(self, tokens: list[Token]):
        self.tokens = tokens
        self.texts = [token.text for token in tokens]

    def __str__(self) -> str:
        return " ".join(self.texts)

    def __len__(self) -> int:
        return len(self.texts)

    def find_all(self, pattern: str) -> list[int]:
        """Start index of every occurrence of `pattern`, as tokens."""

        wanted = words(pattern)
        size = len(wanted)

        return [
            index
            for index in range(len(self.texts) - size + 1)
            if self.texts[index : index + size] == wanted
        ]

    def find(self, pattern: str) -> int:
        """Start index of the first occurrence of `pattern`, or -1."""

        found = self.find_all(pattern)
        return found[0] if found else -1

    def index(self, pattern: str) -> int:
        """Like find(), but a missing pattern is an assertion failure."""

        position = self.find(pattern)
        assert position >= 0, f"expected to find `{pattern}` in: {self}"
        return position

    def contains(self, pattern: str) -> bool:
        return self.find(pattern) >= 0

    def count(self, pattern: str) -> int:
        return len(self.find_all(pattern))

    def slice(self, start: int, stop: int) -> "Code":
        return Code(self.tokens[start:stop])

    def block_span(self, open_index: int) -> tuple[int, int]:
        """(first, last) token indexes of the brace block opened at open_index."""

        assert self.texts[open_index] == "{", f"no block opens at token {open_index}"
        return open_index, _closing(self.texts, open_index)

    def blocks_after(self, pattern: str) -> list[tuple[int, int]]:
        """The braced block that directly follows each occurrence of `pattern`."""

        spans = []

        for start in self.find_all(pattern):
            open_index = start + len(words(pattern))

            if open_index < len(self.texts) and self.texts[open_index] == "{":
                spans.append(self.block_span(open_index))

        return spans

    def block_after(self, pattern: str) -> "Code":
        """Body of the one braced block that follows `pattern`."""

        spans = self.blocks_after(pattern)
        assert len(spans) == 1, (
            f"expected exactly one block after `{pattern}`, found {len(spans)}"
        )
        first, last = spans[0]
        return self.slice(first + 1, last)

    def strings(self) -> list[str]:
        return [token.text for token in self.tokens if token.kind == "string"]

    def call_arguments(self, name: str) -> list["Code"]:
        """The argument tokens of every call to `name` in this code."""

        return [
            self.slice(start + 2, _closing(self.texts, start + 1))
            for start in self.find_all(f"{name} (")
        ]

    def without_serial_output(self) -> "Code":
        """This code with every `Serial.print(...)` / `Serial.println(...)`
        statement removed, so what is left is what the code DOES."""

        kept: list[Token] = []
        index = 0

        while index < len(self.texts):
            if (
                self.texts[index : index + 2] == ["Serial", "."]
                and index + 3 < len(self.texts)
                and self.texts[index + 2] in ("print", "println")
                and self.texts[index + 3] == "("
            ):
                close = _closing(self.texts, index + 3)
                assert self.texts[close + 1] == ";", (
                    f"Serial output used as an expression at {self.tokens[index].where}"
                )
                index = close + 2
                continue

            kept.append(self.tokens[index])
            index += 1

        return Code(kept)


@dataclass(frozen=True)
class Function:
    name: str
    body: Code
    where: str
    # Token indexes in the sketch: the name, and the brace that closes the body.
    name_index: int
    close_index: int


class Sketch:
    """Every source file Arduino compiles for the sketch, read as one."""

    def __init__(self, files: list[Path]):
        self.files = files
        tokens: list[Token] = []

        for path in files:
            tokens.extend(
                tokenize(path.read_text(encoding="utf-8"), str(path.relative_to(PROJECT_ROOT)))
            )

        self.code = Code(tokens)
        self.functions = self._index_functions()

    def _index_functions(self) -> dict[str, list[Function]]:
        found: dict[str, list[Function]] = {}
        self._scan(0, len(self.code), found)
        return found

    def _scan(self, start: int, stop: int, found: dict[str, list[Function]]) -> None:
        texts = self.code.texts
        index = start

        while index < stop:
            text = texts[index]

            # Namespaces and extern "C" blocks are transparent: a module may wrap
            # its functions in one, and they must still be found.
            if text == "namespace" or (
                text == "extern" and index + 1 < stop and texts[index + 1].startswith('"')
            ):
                open_index = texts.index("{", index)
                close_index = _closing(texts, open_index)
                self._scan(open_index + 1, close_index, found)
                index = close_index + 1
                continue

            if (
                self.code.tokens[index].kind == "ident"
                and index + 1 < stop
                and texts[index + 1] == "("
                and (index == 0 or texts[index - 1] not in (".", "->", "::"))
            ):
                after = _closing(texts, index + 1) + 1

                while after < stop and texts[after] in _BODY_QUALIFIERS:
                    after += 1

                if after < stop and texts[after] == "{":
                    close_index = _closing(texts, after)
                    found.setdefault(text, []).append(
                        Function(
                            text,
                            self.code.slice(after + 1, close_index),
                            self.code.tokens[index].where,
                            index,
                            close_index,
                        )
                    )
                    index = close_index + 1
                    continue

            # Any other top-level block (struct, enum, initializer) is skipped
            # whole, so nothing inside it is mistaken for a definition.
            if text == "{":
                index = _closing(texts, index) + 1
                continue

            index += 1

    def _only_definition(self, name: str) -> Function:
        definitions = self.functions.get(name, [])
        searched = ", ".join(str(path.relative_to(PROJECT_ROOT)) for path in self.files)

        if not definitions:
            raise LookupError(f"{name}() is not defined in any sketch file ({searched})")

        if len(definitions) > 1:
            places = ", ".join(definition.where for definition in definitions)
            raise LookupError(f"{name}() is defined more than once: {places}")

        return definitions[0]

    def function(self, name: str) -> Code:
        """The body of the single definition of `name`, wherever it lives."""

        return self._only_definition(name).body

    def definition(self, name: str, returns: str) -> Code:
        """The whole definition of `name`: return type, parameters and body.

        The caller states the return type, and it must match the source
        exactly, so a changed signature fails here by name instead of being
        compiled into something the caller did not expect.
        """

        found = self._only_definition(name)
        start = found.name_index - len(words(returns))
        actual = self.code.texts[start : found.name_index]

        assert actual == words(returns), (
            f"{name}() at {found.where} returns `{' '.join(actual)}`, expected `{returns}`"
        )

        return self.code.slice(start, found.close_index + 1)

    def type_definition(self, keyword: str, name: str) -> Code:
        """`keyword name ... { ... };` whole, for one enum or struct definition.

        A forward declaration (`keyword name;`) is not a definition and is
        skipped. Anything other than exactly one definition is an error.
        """

        texts = self.code.texts
        found: list[Code] = []

        for index in range(len(texts) - 1):
            if texts[index] != keyword or texts[index + 1] != name:
                continue

            cursor = index + 2

            while cursor < len(texts) and texts[cursor] not in ("{", ";"):
                cursor += 1

            if cursor == len(texts) or texts[cursor] == ";":
                continue

            close = _closing(texts, cursor)
            assert close + 1 < len(texts) and texts[close + 1] == ";", (
                f"{keyword} {name} at {self.code.tokens[index].where} "
                "is not followed by `;`"
            )
            found.append(self.code.slice(index, close + 2))

        assert len(found) == 1, (
            f"expected exactly one definition of {keyword} {name}, found {len(found)}"
        )
        return found[0]

    def functions_containing(self, pattern: str) -> set[str]:
        """Names of the functions whose bodies contain `pattern`."""

        return {
            name
            for name, definitions in self.functions.items()
            for definition in definitions
            if definition.body.contains(pattern)
        }

    def callers(self, name: str) -> set[str]:
        """Names of the functions whose bodies call `name`."""

        return self.functions_containing(f"{name} (")

    def struct(self, name: str) -> Code:
        """The member declarations of `struct ... name { ... };`."""

        texts = self.code.texts
        matches = [
            index
            for index in range(len(texts) - 1)
            if texts[index] == name
            and texts[index + 1] == "{"
            and "struct" in texts[max(0, index - 8) : index]
        ]
        assert len(matches) == 1, f"expected one definition of struct {name}"
        first, last = self.code.block_span(matches[0] + 1)
        return self.code.slice(first + 1, last)

    def struct_head(self, name: str) -> Code:
        """Tokens from `struct` up to the name, where attributes live."""

        texts = self.code.texts
        end = next(
            index
            for index in range(len(texts) - 1)
            if texts[index] == name and texts[index + 1] == "{"
        )
        start = max(index for index in range(end) if texts[index] == "struct")
        return self.code.slice(start, end + 1)


def sketch_files() -> list[Path]:
    """The files Arduino CLI compiles: the sketch root, plus src/ recursively."""

    files = [
        path
        for path in SKETCH_DIR.iterdir()
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    ]

    source_dir = SKETCH_DIR / "src"

    if source_dir.is_dir():
        files.extend(
            path
            for path in source_dir.rglob("*")
            if path.is_file() and path.suffix in SOURCE_SUFFIXES
        )

    if not any(path.suffix == ".ino" for path in files):
        raise FileNotFoundError(f"no .ino sketch found in {SKETCH_DIR}")

    return sorted(files)


@functools.cache
def load() -> Sketch:
    return Sketch(sketch_files())
