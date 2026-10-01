#!/usr/bin/env python3
"""测试用静态服务器（带 CORS 头）——替代 python -m http.server"""
import http.server
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8899

class CORSHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

http.server.test(HandlerClass=CORSHandler, port=PORT)
