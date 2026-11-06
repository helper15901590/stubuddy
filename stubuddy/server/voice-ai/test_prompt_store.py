import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import prompt_store as prompts
import reminder_tools
import install_ecs
import configure
import prompt_admin


class PromptTests(unittest.IsolatedAsyncioTestCase):
    def test_admin_invalid_candidate_preserves_active_and_does_not_restart(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            active = root / '.prompt-overrides.json'
            active.write_text('{}')
            candidate = root / 'candidate.json'
            candidate.write_text('{"invalid":"fixture"}')
            with mock.patch.object(prompt_admin, 'ROOT', root), mock.patch.object(prompt_admin, 'authorize'), mock.patch.object(prompt_admin, 'write', side_effect=lambda path, data, *args: path.write_bytes(data)), mock.patch.object(prompt_admin, 'restart') as restart:
                with self.assertRaises(prompts.PromptError): prompt_admin.apply(candidate)
                restart.assert_not_called()
            self.assertEqual(active.read_text(), '{}')

    def test_admin_failed_restart_restores_previous_override(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            active = root / '.prompt-overrides.json'
            active.write_text('{}')
            candidate = root / 'candidate.json'
            candidate.write_text(json.dumps({'device_tools.reply.answer_with_tools.010': '你好呀！'}), encoding='utf-8')
            with mock.patch.object(prompt_admin, 'ROOT', root), mock.patch.object(prompt_admin, 'authorize'), mock.patch.object(prompt_admin, 'write', side_effect=lambda path, data, *args: path.write_bytes(data)), mock.patch.object(prompt_admin, 'restart', side_effect=[RuntimeError(), None]) as restart:
                with self.assertRaises(RuntimeError): prompt_admin.apply(candidate)
                self.assertEqual(restart.call_count, 2)
            self.assertEqual(active.read_text(), '{}')

    def test_invalid_overrides_are_rejected_without_echoing_values(self):
        key = 'device_tools.reply.answer_with_tools.010'
        for raw in ('{"unknown":"fixture-secret"}', '{', '[]',
                    json.dumps({key: ''}), json.dumps({key: 1}), json.dumps({key: '{p0}'}),
                    json.dumps({key: 'x' * 5000}), '{"x":"one","x":"two"}'):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'patch.json'
                path.write_text(raw, encoding='utf-8')
                with self.assertRaises(prompts.PromptError) as error: prompts.load(overrides=path)
                self.assertNotIn('fixture-secret', str(error.exception))

    def test_snapshot_requires_reload_and_preserves_defaults(self):
        key = 'device_tools.reply.answer_with_tools.010'
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'patch.json'
            path.write_text(json.dumps({key: '你好，今天一起学点什么？'}, ensure_ascii=False), encoding='utf-8-sig')
            first = prompts.load(overrides=path)
            with mock.patch.object(prompts, '_snapshot', first):
                path.write_text('{}', encoding='utf-8')
                self.assertEqual(prompts.wording(key), '你好，今天一起学点什么？')
            second = prompts.load(overrides=path)
            self.assertNotEqual(first[2], second[2])
            self.assertNotEqual(first[0][key], second[0][key])

    async def test_changed_clarification_does_not_change_reminder_flow(self):
        values, _, version = prompts.load(overrides=Path('does-not-exist-prompt-test.json'))
        values = dict(values)
        values['reminder_tools.reply.clock_value.002'] = '请补充时刻。'
        values['reminder_tools.reply.clock_value.005'] = '请补充上午还是下午。'
        from test_reminders import status, provider_plan
        execute, prepare, view = mock.AsyncMock(return_value=status()), mock.AsyncMock(), {}
        with mock.patch.object(prompts, '_snapshot', (values, frozenset(), version)):
            answer = await reminder_tools.answer(provider_plan(), [{'content': '每周一七点提醒我喝水'}], execute, prepare, view)
            self.assertEqual(answer, '请补充上午还是下午。')
            self.assertIn('clock_fragment', view['draft'])
            self.assertIsNone(view['draft']['minute'])
            prepare.assert_not_called()

    def test_placeholder_field_and_format_are_preserved(self):
        key = 'reminders.reply.list_entry'
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'patch.json'
            for text in ('{p0.__class__}', '{p0}{p1}', '{p0}{p1}{p2}{p3}{p4}{p4}'):
                path.write_text(json.dumps({key: text}), encoding='utf-8')
                with self.assertRaises(prompts.PromptError): prompts.load(overrides=path)
            path.write_text(json.dumps({key: '{p0}，{p1}，{p2}{p3}{p4}。'}), encoding='utf-8')
            prompts.load(overrides=path)

    def test_package_and_redeploy_preserve_override(self):
        self.assertIn('prompts/defaults.json', configure.FILES)
        self.assertNotIn('.prompt-overrides.json', configure.FILES)
        with tempfile.TemporaryDirectory() as folder:
            root, target = Path(folder) / 'upload', Path(folder) / 'target'
            target.mkdir()
            for name in install_ecs.FILES:
                file = root / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text('fixture', encoding='utf-8')
            override = target / '.prompt-overrides.json'
            override.write_text('{"retained":"fixture"}', encoding='utf-8')
            with mock.patch.object(install_ecs, 'ROOT', root), mock.patch.object(install_ecs, 'TARGET', target):
                install_ecs.copy_payload()
            self.assertEqual(override.read_text(), '{"retained":"fixture"}')

    def test_preflight_rejects_invalid_patch_before_system_changes(self):
        with mock.patch.object(install_ecs, 'validate_prompts', side_effect=prompts.PromptError('Invalid patch')), mock.patch.object(install_ecs, 'run') as run:
            with self.assertRaises(prompts.PromptError): install_ecs.preflight()
            run.assert_not_called()
