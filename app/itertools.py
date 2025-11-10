# Minimal subset of itertools for MicroPython: chain, islice, tee

class chain:
    def __init__(self, *iterables):
        self._iterables = iterables
        self._index = 0
        self._current = None

    def __iter__(self):
        return self

    def __next__(self):
        while True:
            if self._current is None:
                if self._index >= len(self._iterables):
                    raise StopIteration
                self._current = iter(self._iterables[self._index])
            try:
                return next(self._current)
            except StopIteration:
                self._current = None
                self._index += 1

    @classmethod
    def from_iterable(cls, iterable_of_iterables):
        return cls(*iterable_of_iterables)

def islice(iterable, *args):
    if not args:
        raise TypeError("islice expected at least 2 arguments (start, stop)")
    it = iter(iterable)
    if len(args) == 1:
        start = 0
        stop = args[0]
        step = 1
    elif len(args) == 2:
        start, stop = args
        step = 1
    elif len(args) == 3:
        start, stop, step = args
    else:
        raise TypeError("islice(iterable, start, stop[, step])")
    if start < 0:
        start = 0
    if step <= 0:
        raise ValueError("step must be > 0")

    # Skip to start
    for _ in range(start):
        try:
            next(it)
        except StopIteration:
            return
    index = start
    while stop is None or index < stop:
        try:
            val = next(it)
        except StopIteration:
            return
        yield val
        index += step
        # Advance step-1 items
        for _ in range(step - 1):
            if stop is not None and index >= stop:
                return
            try:
                next(it)
            except StopIteration:
                return
            index += 1

def tee(iterable, n=2):
    if n < 1:
        return []
    it = iter(iterable)
    buffers = [[] for _ in range(n)]

    def gen(my_index):
        buf = buffers[my_index]
        while True:
            if buf:
                yield buf.pop(0)
            else:
                try:
                    value = next(it)
                except StopIteration:
                    return
                # Append to all buffers
                for b in buffers:
                    b.append(value)
                # Pop our own
                yield buf.pop(0)

    return [gen(i) for i in range(n)]