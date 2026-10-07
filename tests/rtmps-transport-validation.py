"""Owned numeric-loopback TLS faults; no OS trust/configuration changes.

The linked Schannel backend cannot use ca_file as a private trust store. Thus
certificate rejection is a required security gate, not an established-session
test. Never turn tls_verify off to make an encrypted media test pass.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile


def run_case(probe, mode, cert, key):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        listener.settimeout(4)
        port = listener.getsockname()[1]
        args = [probe, mode, str(port)]
        if mode == "cafile":
            args.append(str(cert))
        if mode == "isolation":
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                args.append(str(reservation.getsockname()[1]))
        # Do not inherit a proxy pointing outside the isolated fixture.
        env = {k: v for k, v in os.environ.items()
               if k.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy")}
        env["no_proxy"] = "*"
        child = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, env=env)
        events = []
        peers = []
        try:
            for _ in range(3 if mode == "sender-stop" else 2 if mode == "isolation" else 1):
                peer, address = listener.accept()
                peers.append(peer)
                peer.settimeout(3)
                assert address[0] == "127.0.0.1"
                hello = peer.recv(5, socket.MSG_PEEK)
                assert len(hello) == 5 and hello[0] == 22 and hello[1] == 3, "no TLS ClientHello"
                events.append({"client_hello": True})
                if mode in ("reject", "cafile", "owner-reject", "isolation"):
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(cert, key)
                    tls = context.wrap_socket(peer, server_side=True, do_handshake_on_connect=False)
                    peers.append(tls)
                    try:
                        tls.do_handshake()
                        events[-1]["negotiated"] = tls.version()
                        data = tls.recv(1537)
                        events[-1]["decrypted_application_bytes"] = len(data)
                    except ssl.SSLError as error:
                        events[-1]["tls_alert"] = str(error)
                    except ConnectionError as error:
                        events[-1]["peer_closed"] = str(error)
                    tls.close()
                elif mode == "drop":
                    peer.close()  # Drop after real ClientHello, before TLS established.
                elif mode in ("cancel", "sender-stop", "owner-stop"):
                    child.stdin.write("stop\n")
                    child.stdin.flush()
                # deadline deliberately withholds ServerHello with socket open.
            output, _ = child.communicate(timeout=8)
            result = {"mode": mode, "port": port, "events": events,
                      "exit": child.returncode, "output": output}
            print(json.dumps(result), flush=True)
            assert child.returncode == 0, output
            if mode in ("reject", "cafile", "owner-reject", "isolation"):
                assert "0x80090325" in output, "expected Schannel untrusted-root rejection"
                assert not any(event.get("decrypted_application_bytes", 0) for event in events), \
                    "untrusted certificate allowed encrypted RTMP application data"
                assert not any("negotiated" in event for event in events), \
                    "untrusted certificate completed TLS handshake"
            return result
        finally:
            for peer in peers:
                peer.close()
            if child.poll() is None:
                child.kill()  # only this disposable harness, never a production worker
                child.communicate(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("probe")
    parser.add_argument("openssl")
    parser.add_argument("--case", choices=["all", "reject", "cafile", "deadline", "cancel", "drop", "sender-stop", "owner-stop", "owner-reject", "isolation"], default="all")
    args = parser.parse_args()
    # Use TMPDIR when the test runner provides one.
    with tempfile.TemporaryDirectory(prefix="active-delay-tls-", dir=os.environ.get("TMPDIR")) as temp:
        cert, key = Path(temp) / "cert.pem", Path(temp) / "key.pem"
        subprocess.run([args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-subj", "/CN=synthetic-loopback.invalid",
                        "-addext", "subjectAltName=IP:127.0.0.1",
                        "-addext", "basicConstraints=critical,CA:TRUE"],
                       check=True, capture_output=True, timeout=15)
        modes = ["reject", "cafile", "deadline", "cancel", "drop", "sender-stop", "owner-stop", "owner-reject", "isolation"] if args.case == "all" else [args.case]
        results = [run_case(args.probe, mode, cert, key) for mode in modes]
    print(json.dumps({"passed": len(results), "private_cert_directory_removed": not Path(temp).exists()}))


if __name__ == "__main__":
    main()