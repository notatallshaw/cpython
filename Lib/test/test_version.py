import gc
import inspect
import operator
import pickle
import sys
import unittest
import weakref

from test.support import script_helper

from _version import InvalidVersion, Version


class DisplayVersion(Version):
    def __init__(self, text):
        self.text = text


class VersionTests(unittest.TestCase):
    def test_normalization_and_properties(self):
        cases = {
            "v1.0": "1.0",
            "1.0alpha1": "1.0a1",
            "1.0-c2": "1.0rc2",
            "1.0-3": "1.0.post3",
            "1.0DEV": "1.0.dev0",
            "1!2.0+ABC_001": "1!2.0+abc.1",
            "\u202f1.2.3\u2029": "1.2.3",
        }
        for text, normalized in cases.items():
            with self.subTest(text=text):
                value = Version(text)
                self.assertEqual(str(value), normalized)
                self.assertEqual(value, Version(normalized))
                self.assertEqual(hash(value), hash(Version(normalized)))

        value = Version("2!1.2.3rc4.post5.dev6+local.7")
        self.assertEqual(value.epoch, 2)
        self.assertEqual(value.release, (1, 2, 3))
        self.assertEqual(value.pre, ("rc", 4))
        self.assertEqual(value.post, 5)
        self.assertEqual(value.dev, 6)
        self.assertEqual(value.local, "local.7")
        self.assertEqual(value.public, "2!1.2.3rc4.post5.dev6")
        self.assertEqual(value.base_version, "2!1.2.3")

    def test_ordering_and_hash(self):
        texts = [
            "1.0.dev0", "1.0a0", "1.0a0.post0.dev0", "1.0a0.post0",
            "1.0b1", "1.0rc1", "1.0", "1.0+local", "1.0.post0.dev0",
            "1.0.post0", "2!0",
        ]
        values = list(map(Version, texts))
        self.assertEqual(values, sorted(reversed(values)))
        self.assertEqual(Version("1"), Version("1.0.0"))
        self.assertEqual(hash(Version("1")), hash(Version("1.0.0")))
        self.assertIs(Version("1").__eq__(object()), NotImplemented)
        self.assertIs(Version("1").__ne__(object()), NotImplemented)
        for op in (operator.lt, operator.le, operator.gt, operator.ge):
            with self.assertRaises(TypeError):
                op(Version("1"), object())

    def test_component_factories(self):
        value = Version.from_parts(
            epoch=2, release=(1, 2, 3), pre=("BETA", 4), post=5,
            dev=6, local="ABC_001",
        )
        self.assertEqual(str(value), "2!1.2.3b4.post5.dev6+abc.1")
        changed = value.__replace__(epoch=None, pre=None, local=None)
        self.assertEqual(str(changed), "1.2.3.post5.dev6")
        self.assertIs(changed.__replace__(), changed)
        self.assertIs(changed.__replace__(release=changed.release), changed)
        self.assertIsInstance(
            Version.from_parts(release=(65536,), post=2**80), Version)

    def test_invalid_components(self):
        invalid = (
            {"release": ()}, {"release": (-1,)}, {"release": [1]},
            {"release": (1,), "epoch": -1},
            {"release": (1,), "pre": ("alpha", -1)},
            {"release": (1,), "local": "abc..1"},
        )
        for parts in invalid:
            with self.subTest(parts=parts):
                with self.assertRaises(InvalidVersion):
                    Version.from_parts(**parts)

    def test_subclass_and_pickle(self):
        value = DisplayVersion("1.2.3")
        restored = pickle.loads(pickle.dumps(value))
        self.assertIs(type(restored), DisplayVersion)
        self.assertEqual(restored.text, "1.2.3")
        self.assertEqual(restored, value)

    def test_size_and_cycles(self):
        self.assertLess(sys.getsizeof(Version("1.2.3")),
                        sys.getsizeof(Version("65536.1")))

        class CyclicVersion(Version):
            __slots__ = ("__weakref__",)

        class Component(int):
            pass

        component = Component(65536)
        value = CyclicVersion.from_parts(release=(component,))
        component.owner = value
        reference = weakref.ref(value)
        del value, component
        gc.collect()
        self.assertIsNone(reference())

    def test_signatures(self):
        create = inspect.signature(Version.from_parts)
        change = inspect.signature(Version("1").__replace__)
        self.assertIs(create.parameters["release"].default,
                      inspect.Parameter.empty)
        self.assertEqual(set(create.parameters), set(change.parameters))

    def test_callbacks_do_not_observe_partial_values(self):
        code = r"""
import gc
import sys
from _version import Version
sys.set_int_max_str_digits(0)
def audit(event, args):
    if event == 'import' and args[0] == '_pylong':
        for value in gc.get_objects():
            if type(value) is tuple:
                try:
                    hash(value)
                except Exception:
                    pass
sys.addaudithook(audit)
value = Version('9' * 7000 + '.0')
assert value.release[0] == int('9' * 7000)
"""
        script_helper.assert_python_ok("-X", "faulthandler", "-c", code)


if __name__ == "__main__":
    unittest.main()
