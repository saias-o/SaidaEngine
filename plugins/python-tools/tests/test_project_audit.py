import unittest

from saida_tools.builtins.project_audit import _runtime_scene_issues


class RuntimeSceneAuditTests(unittest.TestCase):
    def test_numeric_runtime_ids_are_valid(self):
        document = {
            "schema": 2,
            "version": 2,
            "scene": {
                "type": "Scene",
                "id": 1,
                "behaviours": [],
                "children": [{
                    "type": "Node",
                    "id": 2,
                    "behaviours": [],
                    "children": [],
                }],
            },
        }
        self.assertEqual(_runtime_scene_issues(document), [])

    def test_authoring_string_id_is_not_a_runtime_id(self):
        document = {
            "schema": 2,
            "version": 2,
            "scene": {"type": "Scene", "id": "1", "children": []},
        }
        issues = _runtime_scene_issues(document)
        self.assertIn("JSON number", issues[0][1])

    def test_duplicate_and_unknown_types_are_reported(self):
        document = {
            "scene": {
                "type": "Scene",
                "id": 7,
                "behaviours": [{"type": "UnknownBehaviour"}],
                "children": [{"type": "UnknownNode", "id": 7}],
            },
        }
        issues = _runtime_scene_issues(
            document,
            allowed_nodes={"Scene", "Node"},
            allowed_behaviours={"ScriptBehaviour"},
        )
        messages = "\n".join(message for _path, message in issues)
        self.assertIn("duplicate node id", messages)
        self.assertIn("unsupported runtime node type", messages)
        self.assertIn("unsupported runtime behaviour type", messages)


if __name__ == "__main__":
    unittest.main()
