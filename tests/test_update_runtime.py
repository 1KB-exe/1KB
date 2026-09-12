"""HTTP integration tests for snapshot plus cumulative-overlay updates."""
import http.server, io, shutil, subprocess, sys, tempfile, threading, zipfile
from pathlib import Path


def archive(files, deleted=()):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(files.items()): z.writestr(name, data)
        if deleted: z.writestr('1kb.updates.ini', ''.join(f'delete={p}\n' for p in sorted(deleted)))
    return out.getvalue()

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path); data = self.server.assets.get(self.path)
        self.send_response(200 if data is not None else 404)
        if data is not None: self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        if data is not None: self.wfile.write(data)
    def log_message(self, *_): pass

def main():
    driver = str(Path(sys.argv[1]).resolve()); exe = Path(driver).read_bytes()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler); server.assets={};server.requests=[]
    threading.Thread(target=server.serve_forever, daemon=True).start(); base=f'http://localhost:{server.server_port}'
    def publish(version, snapshot_name, snapshot_files, changed=None, deleted=()):
        server.assets['/'+snapshot_name]=archive(snapshot_files)
        text=f'version={version}\ndownload={base}/{snapshot_name}\n'
        if changed is not None:
            name=f'app-v{version}-changes.zip';server.assets['/'+name]=archive(changed,deleted);text+=f'changes={base}/{name}\n'
        server.assets['/current.ini']=text.encode()
    def run(root):
        server.requests.clear(); result=subprocess.run([driver,str(root),base+'/current.ini'],capture_output=True,timeout=60)
        assert result.returncode==0,result.stderr.decode(errors='replace');assert not (root/'.update').exists();return server.requests[:]
    try:
      with tempfile.TemporaryDirectory(prefix='1kb-simple-runtime-') as tmp:
        base_files={'app.exe':exe,'A':b'a','B':b'b','unchanged':b'same'}
        publish('1.0.0','app-v1.0.0.zip',base_files)
        root=Path(tmp)/'main';req=run(root);assert '/app-v1.0.0.zip' in req
        assert (root/'current.txt').read_text()=='1.0.0\n'
        publish('1.0.1','app-v1.0.0.zip',base_files,{'app.exe':exe+b'one','A':b'A'},('B',))
        req=run(root);assert '/app-v1.0.1-changes.zip' in req and '/app-v1.0.0.zip' not in req
        assert (root/'1.0.1'/'A').read_bytes()==b'A' and not (root/'1.0.1'/'B').exists()
        # The later package is cumulative: it still carries A and B's deletion.
        publish('1.0.2','app-v1.0.0.zip',base_files,{'app.exe':exe+b'two','A':b'A','C':b'C'},('B',))
        req=run(root);assert '/app-v1.0.2-changes.zip' in req and '/app-v1.0.0.zip' not in req
        jump=Path(tmp)/'jump';publish('1.0.0','app-v1.0.0.zip',base_files);run(jump)
        publish('1.0.2','app-v1.0.0.zip',base_files,{'app.exe':exe+b'two','A':b'A','C':b'C'},('B',))
        req=run(jump);assert '/app-v1.0.2-changes.zip' in req and (jump/'1.0.2'/'C').read_bytes()==b'C'
        # A changed download selects a new full snapshot.
        two={'app.exe':exe+b'v2','new':b'new'};publish('2.0.0','app-v2.0.0.zip',two)
        req=run(root);assert '/app-v2.0.0.zip' in req and (root/'current.txt').read_text()=='2.0.0\n'
        # Simulate interruption after directory rename. No usable current means full repair + overlay.
        (root/'2.0.0').rename(root/'2.0.1');publish('2.0.1','app-v2.0.0.zip',two,{'app.exe':exe+b'fixed','extra':b'x'})
        req=run(root);assert '/app-v2.0.0.zip' in req and '/app-v2.0.1-changes.zip' in req
        assert (root/'current.txt').read_text()=='2.0.1\n' and (root/'2.0.1'/'extra').read_bytes()==b'x'
        assert sorted(p.name for p in root.iterdir())==['1kb.ini','2.0.1','current.txt']
      print('snapshot, cumulative, skipped, in-place, new-snapshot, and repair tests passed')
    finally: server.shutdown();server.server_close()
if __name__=='__main__': main()
