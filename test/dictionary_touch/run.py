#!/usr/bin/env python3
"""Compile production dictionary hold routing/lifecycle/hit tests with host I/O shims.

The source methods are copied verbatim into the probe; only hardware, rendering,
and dictionary I/O are replaced. SDK timing/slop classification is its existing
contract, not reproduced here. Run --sanitize for ASan/UBSan.
"""
import argparse
from pathlib import Path
import subprocess


def method(source, signature, next_signature):
    assert source.count(signature) == 1, signature
    return signature + source.split(signature, 1)[1].split(next_signature, 1)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    dictionary = (repo / 'src/activities/reader/DictionaryWordSelectActivity.cpp').read_text()
    reader = (repo / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
    # Check the integration entry points, not a second stand-in call sequence.
    main_source = (repo / 'src/main.cpp').read_text()
    gesture_dispatch = main_source.split('BA gestureAction = BA::BTN_DEFAULT;', 1)[1].split('const unsigned long activityStartTime', 1)[0]
    assert gesture_dispatch.index('handleReaderWordHold()') < gesture_dispatch.index('gestureEventManager.consumeAction(')
    manager = (repo / 'src/activities/ActivityManager.cpp').read_text()
    dispatch = manager.split('bool ActivityManager::handleReaderWordHold()', 1)[1].split('\n}', 1)[0]
    assert 'drainInput || pendingAction != PendingAction::None || !currentIsReaderActivity()' in dispatch
    entry = reader.split('bool EpubReaderActivity::openDictionary(', 1)[1].split('void EpubReaderActivity::onReaderMenuConfirm', 1)[0]
    assert entry.index('ExclusiveActivityAccess') < entry.index('dictionaryHold_.consume') < entry.index('loadPageFromActiveBuild')
    assert entry.index('prepareLookupAt(touchX, touchY)') < entry.index('suspendBackgroundWork();')
    assert 'dictionaryHold_.reset();' in reader.split('void EpubReaderActivity::startActivityForResult', 1)[1].split('void EpubReaderActivity::', 1)[0]
    assert 'return directLookup;' in (repo / 'src/activities/reader/DictionaryWordSelectActivity.h').read_text()
    code = (Path(__file__).parent / 'Probe.cpp').read_text()
    # Shared token rules are production code as well; the watchdog callback is
    # excluded because it is the only hardware-dependent function in this block.
    methods = dictionary.split('bool isSelectableToken(', 1)[1].split('// Fed to the watchdog', 1)[0]
    methods = 'bool isSelectableToken(' + methods
    # Exercise the real early render gate while replacing only the drawing body.
    methods += method(dictionary, 'void DictionaryWordSelectActivity::render(RenderLock&&)',
                      '  // Differential fast path:') + '  ++renders;\n}\n'
    for a, b in [
        ('uint8_t DictionaryWordSelectActivity::internDrawStyle(', 'const char* DictionaryWordSelectActivity::fragmentText('),
        ('int DictionaryWordSelectActivity::closestInRow(', 'int DictionaryWordSelectActivity::wordAt('),
        ('bool DictionaryWordSelectActivity::prepareLookupAt(', '// Collect the selectable words'),
        ('size_t DictionaryWordSelectActivity::buildWordText(', '// Index of the word'),
        ('int DictionaryWordSelectActivity::wordAt(', 'DictionaryWordSelectActivity::WordTouch DictionaryWordSelectActivity::consumeWordTouch'),
        ('void DictionaryWordSelectActivity::handleDefinitionResult(', 'void DictionaryWordSelectActivity::loop()'),
        ('void DictionaryWordSelectActivity::loop()', 'void DictionaryWordSelectActivity::drawSelectionRange'),
    ]:
        methods += method(dictionary, a, b)
    for a, b in [
        ('ReaderWordHold::Page EpubReaderActivity::dictionaryTouchPage()', 'bool EpubReaderActivity::handleReaderWordHold()'),
        ('bool EpubReaderActivity::handleReaderWordHold()', 'bool EpubReaderActivity::openDictionary('),
    ]:
        methods += method(reader, a, b)
    assert code.count('// PRODUCTION_METHODS') == 1
    (out / 'probe.cpp').write_text(code.replace('// PRODUCTION_METHODS', methods))
    command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-g', '-I', str(repo / 'src/activities/reader'),
               '-I', str(repo / 'lib/GfxRenderer'), str(out / 'probe.cpp'), '-o', str(out / 'probe')]
    if args.sanitize:
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True)
    subprocess.run([str(out / 'probe')], check=True)


if __name__ == '__main__':
    main()
