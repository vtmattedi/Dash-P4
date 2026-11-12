"""Run from the pc/ folder:  python -m unittest discover -s tests -v"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from dashlink import browsers  # noqa: E402


class LinearUrls(unittest.TestCase):
    def test_issue_links_become_app_links(self):
        self.assertEqual(
            browsers.linear_app_url("https://linear.app/wakecap/issue/BRA-147/move-the-general-export"),
            "linear://wakecap/issue/BRA-147/move-the-general-export",
        )
        self.assertEqual(browsers.linear_app_url("https://linear.app/wakecap/project/p-1?x=1#c"), "linear://wakecap/project/p-1?x=1#c")
        self.assertEqual(browsers.linear_app_url("https://LINEAR.APP/w/issue/A-1"), "linear://w/issue/A-1")

    def test_only_linear_dot_app_itself(self):
        for url in ("https://linear.app/", "https://linear.app", "http://linear.app/w/issue/A-1",
                    "https://linear.app.evil.com/w/issue/A-1", "https://notlinear.app/w/issue/A-1",
                    "https://evil.com/linear.app/w/issue/A-1", "linear://w/issue/A-1", "https://meet.google.com/abc"):
            self.assertIsNone(browsers.linear_app_url(url), url)


class Opening(unittest.TestCase):
    URL = "https://linear.app/wakecap/issue/BRA-147/slug"

    def test_the_app_is_used_when_it_is_installed(self):
        with mock.patch.object(browsers, "linear_app_installed", return_value=True), \
             mock.patch.object(browsers, "launch_scheme") as launch, mock.patch.object(browsers.webbrowser, "open") as web:
            note = browsers.open_in_browser(self.URL, "chrome")
        launch.assert_called_once_with("linear://wakecap/issue/BRA-147/slug")
        web.assert_not_called()
        self.assertEqual(note, "Opened in the Linear app")

    def test_the_browser_when_it_is_not(self):
        with mock.patch.object(browsers, "linear_app_installed", return_value=False), \
             mock.patch.object(browsers, "launch_scheme") as launch, mock.patch.object(browsers.webbrowser, "open") as web:
            browsers.open_in_browser(self.URL, "default")
        launch.assert_not_called()
        web.assert_called_once_with(self.URL)

    def test_the_browser_when_the_app_fails_or_is_switched_off(self):
        with mock.patch.object(browsers, "linear_app_installed", return_value=True), \
             mock.patch.object(browsers, "launch_scheme", side_effect=OSError("no handler")), \
             mock.patch.object(browsers.webbrowser, "open") as web:
            browsers.open_in_browser(self.URL, "default")
        web.assert_called_once_with(self.URL)
        with mock.patch.object(browsers, "linear_app_installed", return_value=True), \
             mock.patch.object(browsers, "launch_scheme") as launch, mock.patch.object(browsers.webbrowser, "open") as web:
            browsers.open_in_browser(self.URL, "default", linear_app=False)
        launch.assert_not_called()
        web.assert_called_once_with(self.URL)

    def test_other_links_never_go_to_the_app(self):
        with mock.patch.object(browsers, "linear_app_installed", return_value=True), \
             mock.patch.object(browsers, "launch_scheme") as launch, mock.patch.object(browsers.webbrowser, "open") as web:
            browsers.open_in_browser("https://meet.google.com/abc-defg-hij", "default")
        launch.assert_not_called()
        web.assert_called_once()


if __name__ == "__main__":
    unittest.main()
