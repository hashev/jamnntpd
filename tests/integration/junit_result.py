"""Small dependency-free per-method JUnit recorder for native unittest scenarios."""
from pathlib import Path
import time
import unittest
import xml.etree.ElementTree as ET


class JUnitResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.records = {}
        self.started = {}

    def startTest(self, test):
        self.started[test.id()] = time.monotonic()
        super().startTest(test)

    def mark(self, test, status, reason=''):
        prior = self.records.get(test.id())
        if prior is None or prior['status'] == 'passed':
            self.records[test.id()] = {'status': status, 'reason': reason}

    def addSuccess(self, test):
        self.mark(test, 'passed')
        super().addSuccess(test)

    def addFailure(self, test, error):
        self.mark(test, 'failed', error[0].__name__)
        super().addFailure(test, error)

    def addError(self, test, error):
        self.mark(test, 'failed', error[0].__name__)
        super().addError(test, error)

    def addSkip(self, test, reason):
        self.mark(test, 'skipped', reason)
        super().addSkip(test, reason)

    def addSubTest(self, test, subtest, error):
        if error is not None:
            self.mark(test, 'failed', error[0].__name__)
        else:
            self.mark(test, 'passed')
        super().addSubTest(test, subtest, error)

    def stopTest(self, test):
        if test.id() in self.records:
            self.records[test.id()]['seconds'] = time.monotonic()-self.started[test.id()]
        super().stopTest(test)

    def write(self, path):
        root = ET.Element('testsuite', tests=str(self.testsRun))
        for ident, result in sorted(self.records.items()):
            classname, _, name = ident.rpartition('.')
            case = ET.SubElement(root, 'testcase', classname=classname, name=name,
                                 time=str(result.get('seconds', 0)))
            if result['status'] != 'passed':
                ET.SubElement(case, 'skipped' if result['status'] == 'skipped' else 'failure',
                              message=result['reason'])
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        ET.ElementTree(root).write(path, encoding='utf-8', xml_declaration=True)
