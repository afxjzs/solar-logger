"""A minimal stand-in for a pyserial port, for driving SerialDevice in tests.

Only the four members SerialDevice actually touches are implemented:
`reset_input_buffer`, `write`, `flush`, and `readline`. Anything else is left
undefined on purpose - a test that needs more is a test whose subject has grown
a new dependency on the serial layer, and that should be visible rather than
absorbed by a permissive mock.

`readline()` returns b"" for a read timeout, which is what pyserial does and
what SerialDevice._read_line() turns into None.
"""

from __future__ import annotations


class FakeSerialError(OSError):
    """Raised by a scripted line to simulate the port disappearing.

    Subclasses OSError deliberately. pyserial's SerialException derives from
    IOError, which is OSError, and SerialDevice._read_line() catches exactly
    (serial.SerialException, OSError) before re-raising as DeviceError. A fake
    error outside that set would sail past the handler under test and prove
    nothing about how a real disconnect is handled.
    """


class FakeSerial:
    """Replays a scripted sequence of firmware lines.

    Each element of `lines` is either a str (one firmware line), None (a read
    timeout with no data), or an exception instance (raised from readline).
    """

    def __init__(self, lines=None):
        self._script = list(lines or [])
        self.written = []
        self.reset_count = 0
        self.flush_count = 0
        self.closed = False

    # -- the surface SerialDevice uses ------------------------------------

    def reset_input_buffer(self):
        self.reset_count += 1

    def write(self, payload):
        self.written.append(payload)
        return len(payload)

    def flush(self):
        self.flush_count += 1

    def readline(self):
        if not self._script:
            # Nothing scripted left: behave like a silent but healthy port so a
            # capture ends on its idle timer rather than on an artificial error.
            return b""

        item = self._script.pop(0)

        if isinstance(item, BaseException):
            raise item

        if item is None:
            return b""

        return (item + "\n").encode("utf-8")

    def close(self):
        self.closed = True


def attach(device, fake):
    """Give a SerialDevice a fake port without opening a real one.

    SerialDevice.open() calls serial.Serial(), which needs hardware. Tests
    construct the device and inject the port directly; `_serial` is the single
    attribute open() sets.
    """

    device._serial = fake
    return device
