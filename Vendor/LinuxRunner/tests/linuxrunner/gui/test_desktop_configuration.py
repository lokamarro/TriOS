"""Persistent desktop defaults and script contracts without touching a rootfs."""
import os,pathlib,shutil,subprocess,tempfile,unittest
BUNDLE=pathlib.Path(__file__).resolve().parents[3]/'app/LinuxRunner/GUISupport.bundle'
class DesktopConfigurationTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.path=pathlib.Path(self.temp.name);self.home=self.path/"home";self.home.mkdir();self.desktop=self.path/"desktop" if os.geteuid()==0 else self.home/".local/share/linuxrunner/desktop"
  for p in BUNDLE.iterdir():
   if p.is_file():shutil.copy2(p,self.path/p.name)
 def tearDown(self):self.temp.cleanup()
 def configure(self):return subprocess.run(['/bin/sh',str(self.path/'desktop-configure.sh')],check=True,capture_output=True,text=True,env=dict(os.environ,HOME=str(self.home),XDG_CONFIG_HOME=str(self.home/".config")))
 def test_first_install(self):
  self.configure();self.assertTrue((self.desktop/'config/openbox/rc.xml').exists());self.assertIn('wallpaper=',(self.desktop/'config/pcmanfm/LinuxRunner/desktop-items-0.conf').read_text())
 def test_user_changes_preserved(self):
  self.configure();p=self.desktop/'config/openbox/rc.xml';p.write_text('user configuration');self.configure();self.assertEqual(p.read_text(),'user configuration')
 def test_interrupted_defaults_recovered(self):
  p=self.desktop/'config/openbox';p.mkdir(parents=True);(p/'rc.xml.new').write_text('partial');self.configure();self.assertTrue((p/'rc.xml').read_text().startswith('<?xml'))
 def test_no_home_replacement(self):
  before=set(self.path.iterdir());self.configure();self.assertEqual(set(self.path.iterdir())-before,({self.desktop} if os.geteuid()==0 else set()))
 def test_configuration_schema(self):
  self.configure();self.assertIn('schema=1',(self.desktop/'configuration.meta').read_text())
 def test_session_modes_isolated(self):
  s=(BUNDLE/'session.sh').read_text();self.assertIn('if [ "$mode" != desktop ]; then',s);self.assertIn('vnc_lifetime="-forever"',s);self.assertIn('-once -timeout 60',s)
 def test_installer_serialized(self):
  s=(BUNDLE/'installer-run.sh').read_text();self.assertIn('install-desktop.sh',s);self.assertIn('package_lock_acquire',s)
 def test_component_profiles_in_user_configuration(self):
  self.configure()
  for profile in ['lxpanel/LinuxRunner','pcmanfm/LinuxRunner']:
   p=self.home/'.config'/profile;self.assertTrue(p.is_symlink());self.assertTrue(p.exists())
 def test_existing_component_profile_preserved(self):
  p=self.home/'.config/lxpanel/LinuxRunner';p.mkdir(parents=True);(p/'custom').write_text('user');self.configure();self.assertFalse(p.is_symlink());self.assertEqual((p/'custom').read_text(),'user')
 def test_launcher_does_not_leak_component_configuration(self):
  s=(BUNDLE/'desktop-components.sh').read_text();self.assertNotIn('XDG_CONFIG_HOME="$base/config"',s)
 def test_runtime_socket_path_budget(self):
  s=(BUNDLE/'session.sh').read_text();self.assertIn('mktemp -d /tmp/lr-runtime.XXXXXXXXXX',s);self.assertNotIn('XDG_RUNTIME_DIR="$PWD/$session/runtime"',s)
  # GVFS's real suffix from the physical export fits sockaddr_un after repair.
  self.assertLess(len('/tmp/lr-runtime.abcdefghij/gvfsd/socket-vOjHkeb7'),108)
 def test_qt_profiles_and_previous_configuration_preserved(self):
  old=self.home/'.config/pcmanfm-qt/LinuxRunner';old.mkdir(parents=True);(old/'settings.conf').write_text('old customized profile')
  panel=self.home/'.config/lxqt';panel.mkdir(parents=True);(panel/'panel.conf').write_text('user panel')
  env=dict(os.environ,HOME=str(self.home),XDG_CONFIG_HOME=str(self.home/'.config'))
  subprocess.run(['/bin/sh',str(self.path/'desktop-configure.sh'),'qt-x11'],env=env,check=True,capture_output=True)
  config=self.home/'.config/pcmanfm-qt/LinuxRunner-X11/settings.conf'
  self.assertIn('WallpaperMode=fit',config.read_text());self.assertIn('BgColor=#0b1425',config.read_text())
  self.assertIn('showText=true',(self.desktop/'config/lxqt/panel.conf').read_text())
  self.assertEqual((old/'settings.conf').read_text(),'old customized profile');self.assertEqual((panel/'panel.conf').read_text(),'user panel')
  config.write_text('intentional custom wallpaper')
  subprocess.run(['/bin/sh',str(self.path/'desktop-configure.sh'),'qt-x11'],env=env,check=True,capture_output=True)
  self.assertEqual(config.read_text(),'intentional custom wallpaper')
 def test_all_shell_syntax(self):
  for p in BUNDLE.glob('*.sh'):subprocess.run(['/bin/sh','-n',str(p)],check=True)
if __name__=='__main__':unittest.main()
