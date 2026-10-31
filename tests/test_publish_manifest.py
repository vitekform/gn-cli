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
        names=['gn-cli-linux-x64','gn-cli-linux-arm64','gn-cli-linux-armv7','gn-cli-linux-riscv64','gn-cli-linux-ppc64le','gn-cli-linux-s390x',
               'gn-cli-macos-arm64','gn-cli-macos-x64','gn-cli-windows-x64.exe','gn-cli-windows-arm64.exe','gn-cli-windows-x86.exe']
        result=module.manifest(release(*names))['variants']
        self.assertEqual(names,[v['defaultFile'] for v in result])
        self.assertEqual([{'os':n.split('-')[2],'architecture':n.split('-')[3].removesuffix('.exe')} for n in names], [v['selection'] for v in result])
        self.assertEqual({'linux':'armv7','macos':'x64','windows':'x86'}, {v['selection']['os']:v['selection']['architecture'] for v in result if v['key'] in ('linux-armv7','macos-x64','windows-x86')})
        self.assertTrue(all(v['files']==[v['defaultFile']] for v in result))

    def test_incomplete_ambiguous_duplicate_and_unfinished_fail(self):
        cases=[release('gn-cli-linux-x64'), release('gn-cli'), release('gn-cli-linux-x64','gn-cli-linux-x64'), release('gn-cli-windows-x64'),
               release('gn-cli-linux-x64','gn-cli-windows-x64.exe'), release('gn-cli-linux-arm64.exe'), release('gn-cli-windows-arm64'),
               release('gn-cli-linux-mips'), release('gn-cli-macos-armv7'), release(*[f"gn-cli-{p}{'.exe' if p.startswith('windows') else ''}" for p in sorted(module.PLATFORMS) if p != 'linux-s390x'])]
        unfinished=release('gn-cli-linux-x64','gn-cli-linux-arm64','gn-cli-windows-x64.exe','gn-cli-windows-arm64.exe')
        unfinished['files'][1]['status']='UPLOADING'
        cases.append(unfinished)
        for case in cases:
            with self.subTest(case=case),self.assertRaises(ValueError): module.manifest(case)
        with self.assertRaisesRegex(ValueError,'Missing build.*linux-s390x'): module.manifest(cases[-2])


if __name__=='__main__': unittest.main()
