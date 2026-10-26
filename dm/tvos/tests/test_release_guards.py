#!/usr/bin/env python3
"""Static/state helpers for tvOS fail-closed release behavior."""
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import tempfile


def main():
    tv = Path(__file__).resolve().parents[1]
    main_source = (tv / 'Sources/main_tv.m').read_text()
    resource_source = (tv / 'Sources/resources_tv.m').read_text()
    front_source = (tv / 'Sources/front_tv.m').read_text()
    prepare_path = tv / 'tools/prepare_project.py'
    prepare_source = prepare_path.read_text()

    assert 'NSString *msl = imported ? externalMSL : bundledMSL;' in main_source
    assert '!imported && (archiveAvailable || !bundledDirectory)' in main_source
    assert 'if (![fm fileExistsAtPath:msl])' not in main_source
    assert 'preparation_started = 0; // a later foreground/start event may retry' in main_source
    assert 'host_started = 0;' in main_source
    assert main_source.count('pthread_attr_setstacksize') >= 2
    assert 'if (!root.preparationLabel)' in front_source

    assert 'static BOOL publish_marker' in resource_source
    marker = resource_source.index('publish_marker(dest, expectedSHA256)')
    remove_archive = resource_source.index('[fm removeItemAtPath:archivePath')
    assert marker < remove_archive
    assert 'DM_TV_RESOURCE_TESTS' in resource_source
    assert "p.add_argument('--shader-archive-sha256', required=True)" in prepare_source

    spec = spec_from_file_location('prepare_project', prepare_path)
    module = module_from_spec(spec); spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix='tv-prepare-links-') as temporary:
        root = Path(temporary)
        first = root / 'first'; second = root / 'second'
        first.mkdir(); second.mkdir()
        link = root / 'Sources'
        link.symlink_to(root / 'missing', target_is_directory=True)
        module.replace_symlink(link, first, directory=True)
        assert link.resolve() == first.resolve()
        link.unlink(); link.mkdir(); (link / 'stale').write_text('old')
        module.replace_symlink(link, second, directory=True)
        assert link.is_symlink() and link.resolve() == second.resolve()
    print('tvOS release guards: PASS')


if __name__ == '__main__':
    main()
