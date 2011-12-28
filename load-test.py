#!/usr/bin/python

import socket
from urllib.parse import urlencode
from multiprocessing import Process

host = ('localhost', 34000)
conf_site_password = b'*' * 32

def client():
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(host)
        
        # The client sends at most conf->max_read_buffer bytes.
        buf = b''.join([
                b'GET /',
                # passkey := user_id | site-password
                conf_site_password,
                b'/',
                # action := update | scrape | announce
                b'update',
                b' ',
                # params (urlencoded list of arguments)
                update_params(),
                b'HTTP/1.1 \r\n',
                # http headers
                b'user-agent: load-test.py\r\n'
        ])
        assert sock.send(buf) == len(buf)
        assert b'success' in sock.recv(4096)

def update_params():
        # XXX: Generate actual parameters.
        return urlencode({
                'foo': 'bar',
        }).encode()

def test():
        while True:
                client()

if __name__ == '__main__':
        nr = 64
        for k in range(nr-1):
                p = Process(target=test)
                p.start()
        test()
