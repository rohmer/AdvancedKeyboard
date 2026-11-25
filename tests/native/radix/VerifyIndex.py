from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[3]
path = source / 'tools/layout/build_ngtr_index.py'
spec = importlib.util.spec_from_file_location('radix_index', path)
index = importlib.util.module_from_spec(spec)
spec.loader.exec_module(index)
with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    subprocess.run([sys.argv[1], str(directory)], check=True)
    for name in ('legacy', 'radix', 'scored', 'wide'):
        trie = (directory / (name + '.ngtr')).read_bytes()
        expected = (directory / (name + '.ngix')).read_bytes()
        actual = index.build_index(trie)
        if actual != expected:
            raise AssertionError(f'{name}: C++ and Python index bytes differ')
        try:
            index.build_index(trie[:-1])
        except ValueError:
            pass
        else:
            raise AssertionError(f'{name}: truncated model accepted')
    if index.build_index((directory / 'fixed.ngtr').read_bytes()):
        raise AssertionError('Fixed v2 records should not require a sidecar')
print('C++/Python byte-exact indexes passed for legacy v3, radix, scored radix and 64-bit counts; v2 validated without sidecar')
