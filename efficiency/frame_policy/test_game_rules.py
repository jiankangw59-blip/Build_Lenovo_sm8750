"""Check imported game behavior, protocol layout and rejection of unsafe graphs."""
import struct
import unittest

from compile_game_rules import compile_rules

LUA = '''API_VERSION = 4
function set_default_extra_policy(extra_policy, pkg)
    if extra_policy then
        if pkg == "game.wz" or pkg == "game.wz.global" then
            set_extra_policy_rel(0, 6, 0, 0)
        elseif pkg == "game.ys" then
            set_extra_policy_rel(6, 0, -400000, -350000)
        else
            set_extra_policy_rel(0, 6, -150000, -100000)
        end
    else
        remove_extra_policy(0)
        remove_extra_policy(6)
    end
end
'''


class GameRulesTest(unittest.TestCase):
    def test_imports_aliases_and_fps(self):
        fps = [30, 45, 60, 90, 120, 130, 144, 165]
        data, report = compile_rules(LUA, [0, 6], {"game.wz": fps, "game.extra": 120})
        self.assertEqual(len(data), 7556)
        self.assertEqual(struct.unpack_from("<4I", data), (1, 4, 2, 0))
        self.assertEqual(report["rules"]["game.wz"][0], (2, 1, 0, 0))
        self.assertEqual(report["rules"]["game.wz.global"][0], (2, 1, 0, 0))
        self.assertEqual(report["rules"]["game.ys"][1], (2, 0, -400000, -350000))
        self.assertEqual(report["rules"]["game.extra"], report["fallback"])
        self.assertEqual(report["fps_by_package"]["game.wz"], fps)
        self.assertEqual(report["fps_by_package"]["game.extra"], [120])

    def test_rejects_cyclic_relationships(self):
        cycle = LUA.replace("set_extra_policy_rel(0, 6, 0, 0)",
                            "set_extra_policy_rel(0, 6, 0, 0)\nset_extra_policy_rel(6, 0, 0, 0)")
        with self.assertRaises(ValueError):
            compile_rules(cycle, [0, 6])

    def test_rejects_unknown_active_code(self):
        with self.assertRaises(ValueError):
            compile_rules(LUA.replace("set_extra_policy_rel(0, 6, 0, 0)", 'io.open("unsafe", "w")'), [0, 6])

    def test_rejects_invalid_fps(self):
        for fps in ([60, 60], [120, 60], [0], [241], list(range(30, 39))):
            with self.subTest(fps=fps), self.assertRaises(ValueError):
                compile_rules(LUA, [0, 6], {"game.wz": fps})

    def test_rejects_cpu_topology_mismatch(self):
        with self.assertRaises(ValueError):
            compile_rules(LUA, [0, 7])


if __name__ == "__main__":
    unittest.main()
