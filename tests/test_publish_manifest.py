import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('cli_variants', Path(__file__).resolve().parents[1] / 'scripts/rsys_variants.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def release(*names):
    return dict(files=[dict(filename=name, status='READY') for name in names])


class PublishManifestTest(unittest.TestCase):
    def test_complete_manifest_keeps_platform_defaults_separate(self):
        names=['gn-cli-linux-x64','gn-cli-windows-x64.exe','gn-cli-macos-arm64']
        result=module.manifest(release(*names))['variants']
        self.assertEqual(names,[v['defaultFile'] for v in result])
        self.assertEqual([{'os':'linux','architecture':'x64'}, {'os':'windows','architecture':'x64'}, {'os':'macos','architecture':'arm64'}], [v['selection'] for v in result])
        self.assertTrue(all(v['files']==[v['defaultFile']] for v in result))

    def test_incomplete_ambiguous_duplicate_and_unfinished_fail(self):
        cases=[release('gn-cli-linux-x64'), release('gn-cli'), release('gn-cli-linux-x64','gn-cli-linux-x64'), release('gn-cli-windows-x64')]
        unfinished=release('gn-cli-linux-x64','gn-cli-windows-x64.exe')
        unfinished['files'][1]['status']='UPLOADING'
        cases.append(unfinished)
        for case in cases:
            with self.subTest(case=case),self.assertRaises(ValueError): module.manifest(case)


if __name__=='__main__': unittest.main()
