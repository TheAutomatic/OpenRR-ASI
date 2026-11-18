#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import unittest
from compare_runtime_trace import compare, validate_metadata, is_half_surface


def trace(aco=0, groups='2,3,1', indirect=0, push=None, matrix=None, guide='width=1280 height=720'):
    if push is None:push=(4096).to_bytes(8,'little')+(16).to_bytes(4,'little')+bytes(4)
    if matrix is None:matrix=','.join(['1']*16)
    return (f'frame engine=8 phase8=0 fresh=0 reset=0 lite=1 render=1280,720 jitter=0.125,-0.25 motion-scale=1,1 depth-sign=-1\n'
            f'view-to-clip={matrix}\nworld-to-view={matrix}\n'
            f'guide 0 present=1 direct=1 format=1 pitch=10240 origin=0,0 texture-size=1280,720 view-format=10 buffer=0 resource=123\n'
            f'texture input-guide-0 format=10 {guide} row-bytes=10240 origin=0,0\n'
            f'dispatch 0-clear_range aco={aco} indirect={indirect} groups={groups} push={push.hex()}\n')


class TraceDiff(unittest.TestCase):
    def test_packed_history_is_not_half(self):
        self.assertFalse(is_half_surface('1-pooled_history2_lt-pooled-history.bin'))
        self.assertFalse(is_half_surface('input-previous-down.bin'))
        self.assertFalse(is_half_surface('19-dec_head_32_64_f_lt-mvf.bin'))
        self.assertTrue(is_half_surface('input-previous-history.bin'))
        self.assertTrue(is_half_surface('20-post_f_lt-history.bin'))

    def test_zero_sign_and_nonzero_are_separate(self):
        r=compare(bytes.fromhex('0000003c'),bytes.fromhex('0080013c'),True)
        self.assertEqual(r['signed_zero_elements'],1)
        self.assertEqual(r['nonzero_elements'],1)
        self.assertEqual(r['first_nonzero']['element'],1)
        self.assertEqual(r['nonfinite_values'],0)

    def test_nonfinite_checked_even_for_equal_bits(self):
        self.assertEqual(compare(bytes.fromhex('007c'),bytes.fromhex('007c'),True)['nonfinite_values'],2)

    def test_size_mismatch_fails(self):
        with self.assertRaises(ValueError):compare(b'',b'0')

    def test_compiler_marker_only_is_ignored(self):
        self.assertTrue(validate_metadata([trace(),trace(aco=1)])['non_address_parameters_checked'])

    def test_groups_and_indirect_mismatch_fail(self):
        for changed in (trace(groups='3,3,1'),trace(indirect=1)):
            with self.assertRaises(ValueError):validate_metadata([trace(),changed])

    def test_matrix_and_guide_mismatch_fail(self):
        for changed in (trace(matrix=','.join(['2']*16)),trace(guide='width=1088 height=612'),
                        trace().replace('pitch=10240','pitch=8192'),trace().replace('origin=0,0','origin=1,0')):
            with self.assertRaises(ValueError):validate_metadata([trace(),changed])

    def test_non_address_push_change_fails(self):
        with self.assertRaises(ValueError):
            validate_metadata([trace(),trace(push=(4096).to_bytes(8,'little')+(17).to_bytes(4,'little')+bytes(4))])

    def test_address_change_requires_manual_review(self):
        r=validate_metadata([trace(),trace(push=(8192).to_bytes(8,'little')+(16).to_bytes(4,'little')+bytes(4))])
        self.assertTrue(r['address_review_required'])
        self.assertEqual(r['pending_address_pairs'][0]['push_byte'],0)

    def test_missing_dispatch_field_fails(self):
        with self.assertRaises(ValueError):validate_metadata([trace(),trace().replace('indirect=0 ','')])


if __name__=='__main__':unittest.main()
