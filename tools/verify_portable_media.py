"""Exercise the compiled native media module against a deterministic HTTP server.

python tools/verify_portable_media.py build-portable-media
"""
import argparse
import http.server
import io
import math
import os
from pathlib import Path
import struct
import subprocess
import threading
import time
import wave


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build',type=Path)
    args=parser.parse_args()
    output=io.BytesIO()
    with wave.open(output,'wb') as sound:
        sound.setnchannels(2);sound.setsampwidth(2);sound.setframerate(48000)
        sound.writeframes(b''.join(struct.pack('<hh',*[int(math.sin(i*math.tau*220/48000)*400)]*2) for i in range(48000*3)))
    tone=output.getvalue()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                if self.path=='/slow':time.sleep(.6)
                if self.path=='/file-redirect':
                    self.send_response(302);self.send_header('Location','file:///private');self.end_headers();return
                self.send_response(404 if self.path=='/404' else 200)
                body=b'{"station":"local"}' if self.path=='/json' else b'x'*10000 if self.path=='/large' else tone
                self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):pass
        def log_message(self,*args):pass

    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    suffix='.exe' if os.name=='nt' else ''
    base=f'http://127.0.0.1:{server.server_port}'
    try:
        for binary,arguments in [('http_test',[base]),('media_test',[base+'/tone.wav','--finite'])]:
            subprocess.run([str((args.build/(binary+suffix)).resolve()),*arguments],check=True,timeout=40)
    finally:
        server.shutdown();server.server_close();worker.join()


if __name__=='__main__':main()
