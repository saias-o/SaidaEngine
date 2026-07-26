import unittest

from saida_tools.scene import SceneBuilder, SceneNode, Transform, stable_node_id


class SceneBuilderTests(unittest.TestCase):
    def test_stable_ids_and_runtime_envelope(self):
        scene = SceneBuilder("Example")
        scene.add(scene.node(
            "Node", "Marker", transform=Transform(position=(1, 2, 3))))
        document = scene.document()
        self.assertEqual(document["schema"], 2)
        self.assertEqual(document["version"], 2)
        marker = document["scene"]["children"][0]
        self.assertEqual(marker["id"], stable_node_id("Marker"))
        self.assertIsInstance(marker["id"], int)
        self.assertEqual(marker["transform"]["position"], [1, 2, 3])

    def test_duplicate_names_fail_by_default(self):
        scene = SceneBuilder("Example")
        scene.add(SceneNode("Node", "Same"), SceneNode("Node", "Same"))
        with self.assertRaisesRegex(ValueError, "duplicate scene node id"):
            scene.document()

    def test_explicit_numeric_id_remains_numeric(self):
        scene = SceneBuilder("Example")
        scene.add(SceneNode("Node", "Marker", id=42))
        marker = scene.document()["scene"]["children"][0]
        self.assertEqual(marker["id"], 42)

    def test_invalid_explicit_id_is_rejected(self):
        scene = SceneBuilder("Example")
        scene.add(SceneNode("Node", "Marker", id="not-an-id"))  # type: ignore[arg-type]
        with self.assertRaisesRegex(ValueError, "must be an integer"):
            scene.document()


if __name__ == "__main__":
    unittest.main()
