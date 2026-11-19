import json
import tempfile
import unittest
from pathlib import Path

from dashlink.recent import RecentFolders, _local_path


class RecentFoldersTest(unittest.TestCase):
    def test_local_uri_becomes_a_path_and_remote_is_refused(self):
        self.assertIsNone(_local_path("vscode-remote://tunnel%2Bx/d%3A/a"))
        self.assertIsNone(_local_path(None))
        self.assertTrue(_local_path("file:///d%3A/My%20Dir/x").endswith("x"))

    def test_newest_first_one_per_name_and_missing_folders_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            storage = root / "ws"
            for index, name in enumerate(["old", "new", "gone"]):
                folder = root / name
                if name != "gone":
                    folder.mkdir()
                entry = storage / f"id{index}"
                entry.mkdir(parents=True)
                (entry / "workspace.json").write_text(json.dumps({"folder": folder.as_uri()}))
                import os

                os.utime(entry / "workspace.json", (1000 + index * 10 if name != "gone" else 5000,) * 2)
            names = [e["name"] for e in RecentFolders(storage).entries()]
            self.assertEqual(names, ["new", "old"])
            self.assertTrue(all(set(e) == {"name", "where", "open"} for e in RecentFolders(storage).entries()))  # no path leaves the PC

    def test_a_folder_with_a_window_is_tagged_open(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            storage = root / "ws"
            for index, name in enumerate(["alpha", "beta"]):
                (root / name).mkdir()
                entry = storage / f"id{index}"
                entry.mkdir(parents=True)
                (entry / "workspace.json").write_text(json.dumps({"folder": (root / name).as_uri()}))
            recent = RecentFolders(storage, open_windows=lambda: {"beta"})
            self.assertEqual({e["name"]: e["open"] for e in recent.entries()}, {"alpha": False, "beta": True})


if __name__ == "__main__":
    unittest.main()


class LauncherMenuTest(unittest.TestCase):
    def test_menu_tile_lists_choices_and_launches_one_by_name(self):
        import json
        import tempfile
        from pathlib import Path
        from unittest import mock

        from dashlink.launchers import Launchers

        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "launchers.json"
            path.write_text(json.dumps([{"name": "GitHub", "menu": [
                {"name": "One", "url": "https://github.com/a/b"},
                {"name": "Bad", "url": "file:///etc/passwd"},
            ]}]), encoding="utf-8")
            launchers = Launchers(path)
            self.assertEqual(launchers.deck(), [{"name": "GitHub", "icon": "", "menu": [{"name": "One", "icon": "github"}]}])
            with mock.patch("dashlink.browsers.open_in_browser") as opened:
                self.assertTrue(launchers.launch("GitHub", "One")[0])
                opened.assert_called_once()
                self.assertFalse(launchers.launch("GitHub")[0])
                self.assertFalse(launchers.launch("GitHub", "Bad")[0])
