#!/usr/bin/python

import socket
from urllib.parse import urlencode

host = ('localhost', 34000)
conf_site_password = b'*' * 32

class MemBuf:
        def __init__(self, size):
                self.pos = 0
                self.buf = memoryview(bytearray(size))

        def write(self, data):
                new_pos = self.pos + len(data)
                buf[self.pos:new_pos] = data
                self.pos = new_pos

        def tobytes(self):
                return self.buf.tobytes()

def client():
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(host)
        
        action, handler = random.choice([
                (b'scrape', scrape),
                (b'update', update),
                (b'announce', announce),
        ])

        # The client sends at most conf->max_read_buffer bytes.
        buf = MemBuf(4096)
        buf.write(b'GET /')

        if handler != update:
                # XXX: Use real user_id's.
                user_id = b'0' * 32
                buf.write(user_id)
        else:
                buf.write(conf_site_password)

        buf.write(b' ')
        buf.write(action)
        buf.write(b' ')
        buf.write(handler().encode())
        buf.write(b'HTTP/1.1 \r\n')
        buf.write(b'user-agent: load-test.py\r\n')

        sock.send(buf.tobytes())

        # XXX: recv() the whole response.

# XXX: Implement handlers.

def scrape():
        pass

def update():
        pass

def announce():
        pass
