import json,pathlib,socket,subprocess,tempfile,threading,unittest
SCRIPT=pathlib.Path(__file__).with_name('firefox_graphics_report.py')
def send(conn,value):
 raw=json.dumps(value).encode();conn.sendall(str(len(raw)).encode()+b':'+raw)
def read(conn):
 size=b''
 while True:
  b=conn.recv(1)
  if b==b':':break
  if not b:raise EOFError
  size+=b
 raw=b''
 while len(raw)<int(size):raw+=conn.recv(int(size)-len(raw))
 return json.loads(raw)
class GraphicsReport(unittest.TestCase):
 def run_server(self,close=False):
  server=socket.socket();server.bind(('127.0.0.1',0));server.listen();port=server.getsockname()[1]
  def service():
   try:
    conn,_=server.accept()
    with conn:
     conn.settimeout(10);send(conn,{'applicationType':'gecko','marionetteProtocol':3})
     if close:return
     for expected in ['WebDriver:NewSession','WebDriver:Navigate','WebDriver:ExecuteScript','WebDriver:DeleteSession']:
      msg=read(conn);assert msg[2]==expected
      send(conn,[1,msg[1],None,{'value':[{'id':'graphics-features-tbody','text':'fixture software'}]} if 'ExecuteScript' in expected else {}])
   finally:server.close()
  worker=threading.Thread(target=service);worker.start()
  with tempfile.TemporaryDirectory() as d:
   output=pathlib.Path(d)/'report.json'
   result=subprocess.run(['python3',str(SCRIPT),'--port',str(port),'--output',str(output),'--timeout','5'],capture_output=True,timeout=10)
   report=json.loads(output.read_text())
  worker.join(timeout=10);assert not worker.is_alive()
  return result,report
 def test_report(self):
  result,report=self.run_server();self.assertEqual(result.returncode,0);self.assertEqual(report['status'],'PASS');self.assertIn('fixture software',str(report['graphics']))
  self.assertEqual([p['stage'] for p in report['phases']],['connect','greeting','WebDriver:NewSession','WebDriver:Navigate','WebDriver:ExecuteScript','WebDriver:DeleteSession'])
  self.assertTrue(all(p['elapsed_ms']>=0 for p in report['phases']))
 def test_closed_connection(self):
  result,report=self.run_server(True);self.assertNotEqual(result.returncode,0);self.assertEqual(report['status'],'FAIL');self.assertIn('closed',report['error']);self.assertEqual(report['failed_stage'],'WebDriver:NewSession')
 def test_unavailable_reports_connect_stage(self):
  server=socket.socket();server.bind(('127.0.0.1',0));port=server.getsockname()[1]
  with tempfile.TemporaryDirectory() as d:
   output=pathlib.Path(d)/'report.json'
   result=subprocess.run(['python3',str(SCRIPT),'--port',str(port),'--output',str(output),'--timeout','1'],capture_output=True,timeout=4)
   report=json.loads(output.read_text())
  server.close();self.assertNotEqual(result.returncode,0);self.assertEqual(report['failed_stage'],'connect')
if __name__=='__main__':unittest.main()
