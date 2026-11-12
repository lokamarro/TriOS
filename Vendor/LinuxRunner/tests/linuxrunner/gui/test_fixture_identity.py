import copy, hashlib, pathlib, struct, tempfile, unittest
from fixture_identity import check_identity, check_load_bases

class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root=pathlib.Path(self.tmp.name); (self.root/'data/bin').mkdir(parents=True)
        b=bytearray(128); b[:6]=b'\x7fELF\x02\x01'; struct.pack_into('<H',b,18,183); struct.pack_into('<Q',b,32,64); struct.pack_into('<HH',b,54,56,1); struct.pack_into('<IIQQQQQQ',b,64,3,0,120,0,0,8,8,1); b[120:128]=b'/ld.so\0\0'
        self.binary=self.root/'data/bin/app'; self.binary.write_bytes(b)
        self.p={'label':'test','executable':'/bin/app','interpreter':'/ld.so','files':{'/bin/app':hashlib.sha256(b).hexdigest()}}
    def test_matches(self): self.assertEqual(check_identity(self.root,self.p)['interpreter'],'/ld.so')
    def test_changed_binary(self):
        self.binary.write_bytes(self.binary.read_bytes()+b'x')
        with self.assertRaisesRegex(ValueError,'STALE_TRACE_REJECTED'):check_identity(self.root,self.p)
    def test_missing_executable_pin(self):
        self.p['files']={}
        with self.assertRaisesRegex(ValueError,'pinned hash'):check_identity(self.root,self.p)
    def test_interpreter_changed(self):
        self.p['interpreter']='/other'
        with self.assertRaisesRegex(ValueError,'interpreter changed'):check_identity(self.root,self.p)
    def test_traversal(self):
        self.p['files']['/bin/../app']='0'*64
        with self.assertRaisesRegex(ValueError,'Invalid guest'):check_identity(self.root,self.p)
    def test_symlink_escape(self):
        outside=self.root/'outside'; outside.write_text('outside'); (self.root/'data/escape').symlink_to(outside)
        self.p['files']['/escape']=hashlib.sha256(b'outside').hexdigest()
        with self.assertRaisesRegex(ValueError,'escapes fixture'):check_identity(self.root,self.p)
    def test_load_base(self):
        self.p['load_base']='0x1000'
        check_load_bases('000000001000-000000002000 r-xp 00000000 00:00 0 /bin/app',self.p)
    def test_load_base_changed(self):
        self.p['load_bases']={'/lib/library.so':'0x2000'}
        with self.assertRaisesRegex(ValueError,'mapping changed'):
            check_load_bases('000000003000-000000004000 r-xp 00000000 00:00 0 /lib/library.so',self.p)
if __name__=='__main__':unittest.main()
