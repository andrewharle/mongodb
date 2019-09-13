#! /usr/bin/env python3
"""Mock Free Monitoring Endpoint."""

import argparse
import collections
import http.server
import json
import logging
import socketserver
import sys
import urllib.parse

import bson
from bson.codec_options import CodecOptions
from bson.json_util import dumps
import mock_http_common

# Pass this data out of band instead of storing it in FreeMonHandler since the
# BaseHTTPRequestHandler does not call the methods as object methods but as class methods. This
# means there is not self.
stats = mock_http_common.Stats()
last_metrics = None
last_register = None
disable_faults = False
fault_type = None

"""Fault which causes the server to return an HTTP failure on register."""
FAULT_FAIL_REGISTER = "fail_register"

"""Fault which causes the server to return a response with a document with a bad version."""
FAULT_INVALID_REGISTER = "invalid_register"

"""Fault which causes metrics to return halt after 5 metric uploads have occurred."""
FAULT_HALT_METRICS_5 = "halt_metrics_5"

"""Fault which causes metrics to return permanentlyDelete = true after 3 uploads."""
FAULT_PERMANENTLY_DELETE_AFTER_3 = "permanently_delete_after_3"

"""Fault which causes metrics to trigger resentRegistration at 3 uploads."""
FAULT_RESEND_REGISTRATION_AT_3 = "resend_registration_at_3"

"""Fault which causes metrics to trigger resentRegistration once."""
FAULT_RESEND_REGISTRATION_ONCE = "resend_registration_once"

# List of supported fault types
SUPPORTED_FAULT_TYPES = [
    FAULT_FAIL_REGISTER,
    FAULT_INVALID_REGISTER,
    FAULT_HALT_METRICS_5,
    FAULT_PERMANENTLY_DELETE_AFTER_3,
    FAULT_RESEND_REGISTRATION_AT_3,
    FAULT_RESEND_REGISTRATION_ONCE,
]

# Supported POST URL types
URL_POST_REGISTER = '/register'
URL_POST_METRICS = '/metrics'


class FreeMonHandler(http.server.BaseHTTPRequestHandler):
    """
    Handle requests from Free Monitoring and test commands
    """

    def do_GET(self):
        """Serve a Test GET request."""
        parts = urllib.parse.urlsplit(self.path)
        path = parts[2]

        if path == mock_http_common.URL_PATH_STATS:
            self._do_stats()
        elif path == mock_http_common.URL_PATH_LAST_REGISTER:
            self._do_last_register()
        elif path == mock_http_common.URL_PATH_LAST_METRICS:
            self._do_last_metrics()
        elif path == mock_http_common.URL_DISABLE_FAULTS:
            self._do_disable_faults()
        elif path == mock_http_common.URL_ENABLE_FAULTS:
            self._do_enable_faults()
        else:
            self.send_response(http.HTTPStatus.NOT_FOUND)
            self.end_headers()
            self.wfile.write("Unknown URL".encode())

    def do_POST(self):
        """Serve a Free Monitoring POST request."""
        parts = urllib.parse.urlsplit(self.path)
        path = parts[2]

        if path == URL_POST_REGISTER:
            self._do_registration()
        elif path == URL_POST_METRICS:
            self._do_metrics()
        else:
            self.send_response(http.HTTPStatus.NOT_FOUND)
            self.end_headers()
            self.wfile.write("Unknown URL".encode())

    def _send_header(self):
        self.send_response(http.HTTPStatus.OK)
        self.send_header("content-type", "application/octet-stream")
        self.end_headers()

    def _do_registration(self):
        global stats
        global last_register
        clen = int(self.headers.get('content-length'))

        stats.register_calls += 1

        raw_input = self.rfile.read(clen)
        decoded_doc = bson.BSON.decode(raw_input)
        last_register = dumps(decoded_doc)

        if not disable_faults and fault_type == FAULT_FAIL_REGISTER:
            stats.fault_calls += 1
            self.send_response(http.HTTPStatus.INTERNAL_SERVER_ERROR)
            self.send_header("content-type", "application/octet-stream")
            self.end_headers()
            self.wfile.write("Internal Error of some sort.".encode())
            return

        if not disable_faults and fault_type == FAULT_INVALID_REGISTER:
            stats.fault_calls += 1
            data = bson.BSON.encode({
                'version': bson.int64.Int64(42),
                'haltMetricsUploading': False,
                'id': '',
                'informationalURL': 'http://www.example.com/123',
                'message': 'Welcome to the Mock Free Monitoring Endpoint',
                'reportingInterval': bson.int64.Int64(1),
            })
        else:
            data = bson.BSON.encode({
                'version': bson.int64.Int64(1),
                'haltMetricsUploading': False,
                'id': 'mock123',
                'informationalURL': 'http://www.example.com/123',
                'message': 'Welcome to the Mock Free Monitoring Endpoint',
                'reportingInterval': bson.int64.Int64(1),
                'userReminder':
"""To see your monitoring data, navigate to the unique URL below.
Anyone you share the URL with will also be able to view this page.

https://localhost:8080/someUUID6v5jLKTIZZklDvN5L8sZ

You can disable monitoring at any time by running db.disableFreeMonitoring().""",
            })

        self._send_header()

        self.wfile.write(data)

    def _do_metrics(self):
        global stats
        global last_metrics
        clen = int(self.headers.get('content-length'))

        stats.metrics_calls += 1

        raw_input = self.rfile.read(clen)
        decoded_doc = bson.BSON.decode(raw_input)
        last_metrics = dumps(decoded_doc)

        if not disable_faults and \
            stats.metrics_calls > 5 and \
            fault_type == FAULT_HALT_METRICS_5:
            stats.fault_calls += 1
            data = bson.BSON.encode({
                'version': bson.int64.Int64(1),
                'haltMetricsUploading': True,
                'permanentlyDelete': False,
                'id': 'mock123',
                'reportingInterval': bson.int64.Int64(1),
                'message': 'Thanks for all the metrics',
            })
        elif not disable_faults and \
            stats.metrics_calls > 3 and fault_type == FAULT_PERMANENTLY_DELETE_AFTER_3:
            stats.fault_calls += 1
            data = bson.BSON.encode({
                'version': bson.int64.Int64(1),
                'haltMetricsUploading': False,
                'permanentlyDelete': True,
                'id': 'mock123',
                'reportingInterval': bson.int64.Int64(1),
                'message': 'Thanks for all the metrics',
            })
        elif not disable_faults and \
            stats.metrics_calls > 3 and \
            stats.fault_calls < 1 and fault_type == FAULT_RESEND_REGISTRATION_ONCE:
            stats.fault_calls += 1
            data = bson.BSON.encode({
                'version': bson.int64.Int64(2),
                'haltMetricsUploading': False,
                'permanentlyDelete': False,
                'id': 'mock123',
                'reportingInterval': bson.int64.Int64(1),
                'message': 'Thanks for all the metrics',
                'resendRegistration' : True,
            })
        elif not disable_faults and \
            stats.metrics_calls == 3 and fault_type == FAULT_RESEND_REGISTRATION_AT_3:
            stats.fault_calls += 1
            data = bson.BSON.encode({
                'version': bson.int64.Int64(2),
                'haltMetricsUploading': False,
                'permanentlyDelete': False,
                'id': 'mock123',
                'reportingInterval': bson.int64.Int64(1),
                'message': 'Thanks for all the metrics',
                'resendRegistration' : True,
            })
        else:
            data = bson.BSON.encode({
                'version': bson.int64.Int64(1),
                'haltMetricsUploading': False,
                'permanentlyDelete': False,
                'id': '',
                'reportingInterval': bson.int64.Int64(1),
                'message': 'Thanks for all the metrics',
            })

        # TODO: test what if header is sent first?
        self._send_header()

        self.wfile.write(data)

    def _do_stats(self):
        self._send_header()

        self.wfile.write(str(stats).encode('utf-8'))

    def _do_last_register(self):
        self._send_header()

        self.wfile.write(str(last_register).encode('utf-8'))

    def _do_last_metrics(self):
        self._send_header()

        self.wfile.write(str(last_metrics).encode('utf-8'))

    def _do_disable_faults(self):
        global disable_faults
        disable_faults = True
        self._send_header()

    def _do_enable_faults(self):
        global disable_faults
        disable_faults = False
        self._send_header()

def run(port, server_class=http.server.HTTPServer, handler_class=FreeMonHandler):
    """Run web server."""
    server_address = ('', port)

    http.server.HTTPServer.protocol_version = "HTTP/1.1"

    httpd = server_class(server_address, handler_class)

    print("Mock Web Server Listening on %s" % (str(server_address)))

    httpd.serve_forever()


def main():
    """Main Method."""
    global fault_type
    global disable_faults

    parser = argparse.ArgumentParser(description='MongoDB Mock Free Monitoring Endpoint.')

    parser.add_argument('-p', '--port', type=int, default=8000, help="Port to listen on")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    parser.add_argument('--fault', type=str, help="Type of fault to inject")

    parser.add_argument('--disable-faults', action='store_true', help="Disable faults on startup")

    args = parser.parse_args()
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    if args.fault:
        if args.fault not in SUPPORTED_FAULT_TYPES:
            print("Unsupported fault type %s, supports types are %s" % (args.fault, SUPPORTED_FAULT_TYPES))
            sys.exit(1)

        fault_type = args.fault

    if args.disable_faults:
        disable_faults = True

    run(args.port)


if __name__ == '__main__':

    main()
