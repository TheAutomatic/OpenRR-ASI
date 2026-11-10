#!/usr/bin/env python3
"""CPU-only negative tests for source-level address capture validation."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('records', Path(__file__).parents[1]/'build/build_aco_records.py')
records = importlib.util.module_from_spec(spec)
spec.loader.exec_module(records)

class AddressCaptureTests(unittest.TestCase):
    code = struct.pack('<II', 0xbfb00000, 0xbf9f0000)
    report = 'ONR_M0 subst=1 writes=0 constdata=0 pc_relative=0\n'

    def check(self, **changes):
        args = dict(code=self.code, other=self.code, exec_size=4,
                    info={'vgprs':24}, other_info={'vgprs':24}, logs=(self.report,self.report))
        args.update(changes)
        return records.adapt_code(**args)

    def test_identical_pair(self):
        self.assertEqual(self.check()[0], self.code)

    def test_changed_code(self):
        with self.assertRaises(ValueError): self.check(other=b'\0'*8)

    def test_changed_metadata(self):
        with self.assertRaises(ValueError): self.check(other_info={'vgprs':32})

    def test_missing_or_ambiguous_report(self):
        for log in ('',self.report*2):
            with self.assertRaises(ValueError): self.check(logs=(log,self.report))

    def test_forbidden_body_state(self):
        for field in ('writes','constdata','pc_relative'):
            log=self.report.replace(field+'=0',field+'=1')
            with self.assertRaises(ValueError): self.check(logs=(log,log))

    def test_changed_substitution_count(self):
        with self.assertRaises(ValueError): self.check(logs=(self.report,self.report.replace('subst=1','subst=2')))

    def test_constant_tail(self):
        with self.assertRaises(ValueError): self.check(code=struct.pack('<II',0xbfb00000,0))

    def test_unaligned_entry(self):
        with self.assertRaises(ValueError): self.check(exec_size=3)

if __name__ == '__main__': unittest.main()
